#include "upscaler.h"

#include "image_io.h"
#include "resizer.h"
#include "tile_ops.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// Prebuilt ncnn headers (arm64-v8a with Vulkan)
#include "cpu.h"
#include "gpu.h"
#include "net.h"

#define LOG_TAG "EnhancerNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace enhancer {

namespace {

constexpr int kDefaultTileSize = 200;
constexpr int kMinTileSize = 48;
constexpr int kStandardTilePad = 10;
constexpr int kMaxQualityTilePad = 32;
constexpr int kMaxQualityFeatherOut = 32;
constexpr int kMaxQualityHalfFeather = kMaxQualityFeatherOut / 2; // 16 output pixels each side

std::mutex g_vulkan_lifecycle_mutex;
int g_vulkan_ref_count = 0;

class ScopedVulkanInstance {
public:
    ScopedVulkanInstance() {
        std::lock_guard<std::mutex> lock(g_vulkan_lifecycle_mutex);
        if (g_vulkan_ref_count == 0) {
            const int rc = ncnn::create_gpu_instance();
            if (rc != 0) {
                LOGW("ncnn::create_gpu_instance returned %d; falling back to CPU", rc);
            }
        }
        ++g_vulkan_ref_count;
    }

    ~ScopedVulkanInstance() {
        std::lock_guard<std::mutex> lock(g_vulkan_lifecycle_mutex);
        --g_vulkan_ref_count;
        if (g_vulkan_ref_count <= 0) {
            g_vulkan_ref_count = 0;
            ncnn::destroy_gpu_instance();
        }
    }

    ScopedVulkanInstance(const ScopedVulkanInstance&) = delete;
    ScopedVulkanInstance& operator=(const ScopedVulkanInstance&) = delete;
};

struct ModelTensorNames {
    const char* input_name = "data";
    const char* output_name = "output";
};

ModelTensorNames resolve_tensor_names(const std::string& param_path) {
    ModelTensorNames names;
    if (param_path.find("realesr-animevideov3") == std::string::npos) {
        names.input_name = "in0";
        names.output_name = "out0";
    } else {
        names.input_name = "data";
        names.output_name = "output";
    }
    return names;
}

class ScopedModelSession {
public:
    ScopedModelSession() = default;

    ~ScopedModelSession() {
        release();
    }

    ScopedModelSession(const ScopedModelSession&) = delete;
    ScopedModelSession& operator=(const ScopedModelSession&) = delete;

    bool init(
        AAssetManager* mgr,
        const UpscaleConfig& config,
        bool use_vulkan,
        std::string& out_backend_label,
        std::string& out_error
    ) {
        release();
        use_vulkan_ = use_vulkan;

        if (use_vulkan_) {
            const int gpu_idx = ncnn::get_default_gpu_index();
            vkdev_ = (gpu_idx >= 0) ? ncnn::get_gpu_device(gpu_idx) : nullptr;
            if (!vkdev_) {
                out_error = "No Vulkan GPU device available";
                return false;
            }
            blob_vkallocator_ = vkdev_->acquire_blob_allocator();
            staging_vkallocator_ = vkdev_->acquire_staging_allocator();
            if (!blob_vkallocator_ || !staging_vkallocator_) {
                release();
                out_error = "Failed to acquire Vulkan allocators";
                return false;
            }

            net_.opt.use_vulkan_compute = true;
            net_.set_vulkan_device(vkdev_);
            net_.opt.blob_vkallocator = blob_vkallocator_;
            net_.opt.workspace_vkallocator = blob_vkallocator_;
            net_.opt.staging_vkallocator = staging_vkallocator_;

            const char* dev_name = vkdev_->info.device_name();
            out_backend_label = std::string("Vulkan (") + (dev_name ? dev_name : "GPU") + ")";
        } else {
            net_.opt.use_vulkan_compute = false;
            out_backend_label = "CPU (ARM64 OpenMP)";
        }

        const int big_cores = std::max(1, std::min(8, ncnn::get_big_cpu_count()));
        net_.opt.num_threads = big_cores;
        net_.opt.lightmode = true;

        if (config.disable_fp16) {
            net_.opt.use_fp16_packed = false;
            net_.opt.use_fp16_storage = false;
            net_.opt.use_fp16_arithmetic = false;
        } else {
            net_.opt.use_fp16_packed = true;
            net_.opt.use_fp16_storage = true;
            // Keep fp16_arithmetic = false to avoid intermediate feature overflow / NaNs on mobile GPUs
            net_.opt.use_fp16_arithmetic = false;
        }

        const int param_rc = net_.load_param(mgr, config.param_asset_path.c_str());
        if (param_rc != 0) {
            out_error = "Failed to load model param asset: " + config.param_asset_path;
            release();
            return false;
        }

        const int model_rc = net_.load_model(mgr, config.bin_asset_path.c_str());
        if (model_rc != 0) {
            out_error = "Failed to load model bin asset: " + config.bin_asset_path;
            release();
            return false;
        }

        return true;
    }

    void clear_tile_allocators() noexcept {
        if (blob_vkallocator_) {
            blob_vkallocator_->clear();
        }
        if (staging_vkallocator_) {
            staging_vkallocator_->clear();
        }
    }

    void release() noexcept {
        net_.clear();
        if (vkdev_) {
            if (blob_vkallocator_) {
                blob_vkallocator_->clear();
                vkdev_->reclaim_blob_allocator(blob_vkallocator_);
                blob_vkallocator_ = nullptr;
            }
            if (staging_vkallocator_) {
                staging_vkallocator_->clear();
                vkdev_->reclaim_staging_allocator(staging_vkallocator_);
                staging_vkallocator_ = nullptr;
            }
            vkdev_ = nullptr;
        }
    }

    ncnn::Net& net() noexcept { return net_; }

private:
    ncnn::Net net_;
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::VkAllocator* blob_vkallocator_ = nullptr;
    ncnn::VkAllocator* staging_vkallocator_ = nullptr;
    bool use_vulkan_ = false;
};

inline uint8_t clamp_float_255_to_u8(float v) noexcept {
    if (!(v > 0.0f)) {
        return 0;
    }
    const float rounded = v + 0.5f;
    if (rounded >= 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(rounded);
}

inline int reflect_coord(int p, int len) noexcept {
    if (len <= 1) {
        return 0;
    }
    while (p < 0 || p >= len) {
        if (p < 0) {
            p = -p;
        }
        if (p >= len) {
            p = 2 * len - 2 - p;
        }
        if (p < 0 || p >= len) {
            p = std::clamp(p, 0, len - 1);
            break;
        }
    }
    return p;
}

void fill_padded_tile_rgb(
    const uint8_t* work_rgba_or_rgb,
    bool work_is_rgba,
    int work_w,
    int work_h,
    int work_stride_bytes,
    int core_x0,
    int core_y0,
    int pad_left,
    int pad_top,
    int tile_in_w,
    int tile_in_h,
    bool reflect_pad,
    uint8_t* dst_tile_rgb
) {
    for (int ty = 0; ty < tile_in_h; ++ty) {
        const int raw_sy = core_y0 - pad_top + ty;
        const int sy = reflect_pad
            ? reflect_coord(raw_sy, work_h)
            : std::clamp(raw_sy, 0, work_h - 1);
        const uint8_t* src_row =
            work_rgba_or_rgb + static_cast<size_t>(sy) * static_cast<size_t>(work_stride_bytes);
        uint8_t* dst_row = dst_tile_rgb + static_cast<size_t>(ty) * static_cast<size_t>(tile_in_w) * 3;

        if (work_is_rgba) {
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int raw_sx = core_x0 - pad_left + tx;
                const int sx = reflect_pad
                    ? reflect_coord(raw_sx, work_w)
                    : std::clamp(raw_sx, 0, work_w - 1);
                const uint8_t* spx = src_row + static_cast<size_t>(sx) * 4;
                dst_row[tx * 3 + 0] = spx[0];
                dst_row[tx * 3 + 1] = spx[1];
                dst_row[tx * 3 + 2] = spx[2];
            }
        } else {
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int raw_sx = core_x0 - pad_left + tx;
                const int sx = reflect_pad
                    ? reflect_coord(raw_sx, work_w)
                    : std::clamp(raw_sx, 0, work_w - 1);
                const uint8_t* spx = src_row + static_cast<size_t>(sx) * 3;
                dst_row[tx * 3 + 0] = spx[0];
                dst_row[tx * 3 + 1] = spx[1];
                dst_row[tx * 3 + 2] = spx[2];
            }
        }
    }
}

enum class TilePassOutcome {
    OK = 0,
    CANCELLED = 1,
    FAILED_INFERENCE = 2,
};

// Runs 1-pass or 8-pass TTA inference on a prepared `tile_in_w x tile_in_h` RGB888 buffer,
// producing planar float RGB (`tile_out_w x tile_out_h`) scaled to [0..255].
TilePassOutcome run_tile_inference_planar(
    ScopedModelSession& session,
    const ModelTensorNames& tensor_names,
    int model_scale,
    const uint8_t* base_tile_rgb,
    int tile_in_w,
    int tile_in_h,
    bool tta_x8,
    std::vector<uint8_t>& tta_scratch_rgb,
    std::vector<float>& out_plane_r,
    std::vector<float>& out_plane_g,
    std::vector<float>& out_plane_b,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pass_done_0_based)>& on_subpass_done
) {
    const int tile_out_w = tile_in_w * model_scale;
    const int tile_out_h = tile_in_h * model_scale;
    const size_t out_pixels = static_cast<size_t>(tile_out_w) * static_cast<size_t>(tile_out_h);

    try {
        out_plane_r.assign(out_pixels, 0.0f);
        out_plane_g.assign(out_pixels, 0.0f);
        out_plane_b.assign(out_pixels, 0.0f);
    } catch (const std::bad_alloc&) {
        return TilePassOutcome::FAILED_INFERENCE;
    }

    const int pass_count = tta_x8 ? 8 : 1;
    const float scale_255 = 255.0f / static_cast<float>(pass_count);
    static const float kNormVals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};

    for (int t = 0; t < pass_count; ++t) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            return TilePassOutcome::CANCELLED;
        }

        const uint8_t* pass_rgb = base_tile_rgb;
        int pass_in_w = tile_in_w;
        int pass_in_h = tile_in_h;

        if (t != 0) {
            try {
                d4_transform_rgb(base_tile_rgb, tile_in_w, tile_in_h, t, tta_scratch_rgb, pass_in_w, pass_in_h);
            } catch (const std::bad_alloc&) {
                return TilePassOutcome::FAILED_INFERENCE;
            }
            pass_rgb = tta_scratch_rgb.data();
        }

        ncnn::Mat in = ncnn::Mat::from_pixels(
            pass_rgb,
            ncnn::Mat::PIXEL_RGB,
            pass_in_w,
            pass_in_h
        );
        if (in.empty()) {
            return TilePassOutcome::FAILED_INFERENCE;
        }
        in.substract_mean_normalize(nullptr, kNormVals);

        ncnn::Mat out;
        {
            ncnn::Extractor ex = session.net().create_extractor();
            const int in_rc = ex.input(tensor_names.input_name, in);
            if (in_rc != 0) {
                return TilePassOutcome::FAILED_INFERENCE;
            }
            const int ex_rc = ex.extract(tensor_names.output_name, out);
            if (ex_rc != 0 || out.empty() || out.c < 3 ||
                out.w < pass_in_w * model_scale || out.h < pass_in_h * model_scale) {
                return TilePassOutcome::FAILED_INFERENCE;
            }
        }

        const float* ch_r = out.channel(0);
        const float* ch_g = out.channel(1);
        const float* ch_b = out.channel(2);

        d4_inverse_accumulate_planar(
            ch_r,
            ch_g,
            ch_b,
            static_cast<size_t>(out.w),
            pass_in_w * model_scale,
            pass_in_h * model_scale,
            t,
            scale_255,
            out_plane_r.data(),
            out_plane_g.data(),
            out_plane_b.data(),
            tile_out_w,
            tile_out_h
        );

        session.clear_tile_allocators();
        if (on_subpass_done) {
            on_subpass_done(t);
        }
    }

    return TilePassOutcome::OK;
}

} // namespace

std::string query_vulkan_device_info() {
    ScopedVulkanInstance vulkan_guard;
    const int count = ncnn::get_gpu_count();
    const int def_idx = ncnn::get_gpu_device() ? ncnn::get_default_gpu_index() : -1;
    if (count > 0 && def_idx >= 0) {
        const ncnn::GpuInfo& gpu = ncnn::get_gpu_info(def_idx);
        const char* name = gpu.device_name();
        return std::string("Vulkan: ") + (name ? name : "GPU");
    }
    return "CPU only";
}

UpscaleResult run_upscale_pipeline(
    AAssetManager* asset_manager,
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    const std::function<void()>& unlock_bitmap_fn,
    const UpscaleConfig& config,
    const std::atomic<bool>& cancel_flag,
    const ProgressCallback& progress_cb
) {
    UpscaleResult result;

    UniqueFd owned_output_fd(config.output_fd);
    if (!owned_output_fd.valid() || !asset_manager || !src_rgba ||
        src_w <= 0 || src_h <= 0 || src_stride_bytes < src_w * 4 ||
        (config.model_scale != 2 && config.model_scale != 3 && config.model_scale != 4)) {
        result.status = EnhancerStatus::ERR_INVALID_ARGS;
        result.error_detail = "Invalid arguments passed to native upscaler";
        return result;
    }

    const DimensionPlan plan = compute_dimensions(
        src_w,
        src_h,
        config.model_scale,
        config.target_long_side,
        config.max_quality
    );
    if (plan.final_w <= 0 || plan.final_h <= 0) {
        result.status = EnhancerStatus::ERR_INVALID_ARGS;
        result.error_detail = "Computed invalid target dimensions";
        return result;
    }

    result.out_width = plan.final_w;
    result.out_height = plan.final_h;

    int active_tile = (config.requested_tile_size > 0) ? config.requested_tile_size : kDefaultTileSize;
    active_tile = std::max(kMinTileSize, active_tile);
    result.final_tile_size = active_tile;

    const auto report_progress = [&](int pct, const std::string& backend_str) {
        if (progress_cb) {
            progress_cb(std::clamp(pct, 0, 100), backend_str, active_tile);
        }
    };

    if (cancel_flag.load(std::memory_order_relaxed)) {
        result.status = EnhancerStatus::CANCELLED;
        return result;
    }

    // Step 1: Prepare working input buffer (only pre-downscaled when !max_quality && Rule 2)
    NativeByteBuffer work_rgb_buf;
    const uint8_t* work_ptr = src_rgba;
    bool work_is_rgba = true;
    int work_stride = src_stride_bytes;

    if (plan.pre_downscaled) {
        report_progress(2, "Pre-scaling");
        const size_t work_bytes =
            static_cast<size_t>(plan.work_w) * static_cast<size_t>(plan.work_h) * 3;
        work_rgb_buf = allocate_native_buffer(work_bytes);
        if (!work_rgb_buf) {
            result.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            result.error_detail = "Out of memory allocating pre-downscale buffer";
            return result;
        }

        const bool ok_down = downscale_rgba_to_rgb_box(
            src_rgba,
            src_w,
            src_h,
            src_stride_bytes,
            work_rgb_buf.get(),
            plan.work_w,
            plan.work_h,
            cancel_flag
        );
        if (!ok_down) {
            if (cancel_flag.load(std::memory_order_relaxed)) {
                result.status = EnhancerStatus::CANCELLED;
            } else {
                result.status = EnhancerStatus::ERR_INFERENCE;
                result.error_detail = "Pre-downscaling failed";
            }
            return result;
        }
        work_ptr = work_rgb_buf.get();
        work_is_rgba = false;
        work_stride = plan.work_w * 3;
        if (unlock_bitmap_fn) {
            unlock_bitmap_fn();
        }
    }

    // Step 2: Allocate ONLY the final target RGB888 buffer (`final_w * final_h * 3`).
    // Even in Max Quality mode on 12MP-48MP sources, we NEVER allocate the full `model_out` image!
    const size_t final_bytes =
        static_cast<size_t>(plan.final_w) * static_cast<size_t>(plan.final_h) * 3;
    NativeByteBuffer final_rgb = allocate_native_buffer(final_bytes);
    if (!final_rgb) {
        result.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        result.error_detail = "Out of memory allocating target output buffer (" +
            std::to_string(plan.final_w) + "x" + std::to_string(plan.final_h) + ")";
        return result;
    }

    StreamingLanczos3Resizer stream_resizer;
    if (plan.needs_post_resize()) {
        try {
            if (!stream_resizer.init(
                    plan.model_out_w,
                    plan.model_out_h,
                    final_rgb.get(),
                    plan.final_w,
                    plan.final_h
                )) {
                result.status = EnhancerStatus::ERR_INFERENCE;
                result.error_detail = "Failed to initialize streaming Lanczos3 resizer";
                return result;
            }
        } catch (const std::bad_alloc&) {
            result.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            result.error_detail = "Out of memory initializing streaming Lanczos3 resizer";
            return result;
        }
    }

    // Step 3: Initialize Vulkan + Model Session (with automatic CPU fallback)
    ScopedVulkanInstance vulkan_guard;
    const bool gpu_available = (ncnn::get_gpu_count() > 0 && ncnn::get_default_gpu_index() >= 0);
    bool use_vulkan = gpu_available;

    ScopedModelSession session;
    std::string backend_label;
    std::string session_err;

    if (use_vulkan) {
        if (!session.init(asset_manager, config, true, backend_label, session_err)) {
            LOGW("Vulkan session init failed (%s), falling back to CPU", session_err.c_str());
            use_vulkan = false;
            result.used_cpu_fallback = true;
        }
    }

    if (!use_vulkan) {
        session_err.clear();
        if (!session.init(asset_manager, config, false, backend_label, session_err)) {
            result.status = EnhancerStatus::ERR_ASSET_LOAD;
            result.error_detail = session_err;
            return result;
        }
    }

    result.backend_used = backend_label;
    report_progress(4, backend_label);

    const ModelTensorNames tensor_names = resolve_tensor_names(config.param_asset_path);
    const int m = config.model_scale;
    const int progress_start = 5;
    const int progress_end = 92;

    std::vector<uint8_t> tile_rgb;
    std::vector<uint8_t> tta_scratch_rgb;
    std::vector<float> plane_r;
    std::vector<float> plane_g;
    std::vector<float> plane_b;
    std::vector<uint8_t> band_emit_rgb;

    bool inference_done = false;
    while (!inference_done) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }

        if (plan.needs_post_resize()) {
            stream_resizer.reset_stream();
        }

        bool need_retry = false;
        const int passes_per_tile = config.tta_x8 ? 8 : 1;

        auto trigger_tile_fallback = [&](const char* reason) -> bool {
            session.clear_tile_allocators();
            if (active_tile > kMinTileSize) {
                const int next_tile = std::max(kMinTileSize, active_tile / 2);
                LOGW("%s at tile=%d; halving tile size to %d", reason, active_tile, next_tile);
                active_tile = next_tile;
                result.final_tile_size = active_tile;
                return true;
            }
            if (use_vulkan) {
                LOGW("%s at minimum tile=%d on Vulkan; falling back to CPU", reason, active_tile);
                use_vulkan = false;
                result.used_cpu_fallback = true;
                active_tile = (config.requested_tile_size > 0) ? config.requested_tile_size : kDefaultTileSize;
                active_tile = std::max(kMinTileSize, active_tile);
                result.final_tile_size = active_tile;

                std::string cpu_err;
                if (!session.init(asset_manager, config, false, backend_label, cpu_err)) {
                    result.status = EnhancerStatus::ERR_INFERENCE;
                    result.error_detail = "CPU fallback init failed: " + cpu_err;
                    return false;
                }
                result.backend_used = backend_label;
                return true;
            }
            result.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            result.error_detail = std::string(reason) + " even on CPU at minimum tile size 48";
            return false;
        };

        if (config.max_quality) {
            // =========================================================================
            // MAX QUALITY PATH:
            //   - 32px reflection prepadding
            //   - 32px 2D linear feathered overlap blending (exact partition of unity)
            //   - Optional TTA x8 + Optional per-tile adaptive unsharp mask BEFORE blending
            //   - Band-by-band streaming into `final_rgb` or `StreamingLanczos3Resizer`
            // =========================================================================
            const int min_in_cell = std::max(8, (kMaxQualityFeatherOut + m - 1) / m);
            const std::vector<int> x_splits = build_grid_splits(plan.work_w, active_tile, min_in_cell);
            const std::vector<int> y_splits = build_grid_splits(plan.work_h, active_tile, min_in_cell);
            const int nx = static_cast<int>(x_splits.size()) - 1;
            const int ny = static_cast<int>(y_splits.size()) - 1;
            const int total_subpasses = std::max(1, nx * ny * passes_per_tile);
            int completed_subpasses = 0;

            std::vector<float> band_accum;
            std::vector<float> prev_seam_accum;
            if (ny > 1) {
                try {
                    prev_seam_accum.assign(
                        static_cast<size_t>(kMaxQualityFeatherOut) * static_cast<size_t>(plan.model_out_w) * 3,
                        0.0f
                    );
                } catch (const std::bad_alloc&) {
                    if (!trigger_tile_fallback("OOM allocating seam overlap buffer")) {
                        return result;
                    }
                    continue;
                }
            }

            int emitted_model_rows = 0;

            for (int iy = 0; iy < ny && !need_retry; ++iy) {
                const int sy0 = y_splits[static_cast<size_t>(iy)] * m;
                const int sy1 = y_splits[static_cast<size_t>(iy + 1)] * m;
                const int vt = (iy > 0) ? kMaxQualityHalfFeather : 0;
                const int vb = (iy + 1 < ny) ? kMaxQualityHalfFeather : 0;

                const int band_y0 = sy0 - vt;
                const int band_y1 = sy1 + vb;
                const int band_h = band_y1 - band_y0;

                try {
                    band_accum.assign(
                        static_cast<size_t>(band_h) * static_cast<size_t>(plan.model_out_w) * 3,
                        0.0f
                    );
                } catch (const std::bad_alloc&) {
                    if (!trigger_tile_fallback("OOM allocating feathered band buffer")) {
                        return result;
                    }
                    need_retry = true;
                    break;
                }

                for (int ix = 0; ix < nx; ++ix) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }

                    const int sx0 = x_splits[static_cast<size_t>(ix)] * m;
                    const int sx1 = x_splits[static_cast<size_t>(ix + 1)] * m;
                    const int hl = (ix > 0) ? kMaxQualityHalfFeather : 0;
                    const int hr = (ix + 1 < nx) ? kMaxQualityHalfFeather : 0;

                    const int ext_x0 = sx0 - hl;
                    const int ext_x1 = sx1 + hr;
                    const int ext_w = ext_x1 - ext_x0;

                    const int in_x0 = ext_x0 / m;
                    const int in_x1 = (ext_x1 + m - 1) / m;
                    const int in_y0 = band_y0 / m;
                    const int in_y1 = (band_y1 + m - 1) / m;

                    const int core_in_w = in_x1 - in_x0;
                    const int core_in_h = in_y1 - in_y0;

                    const int pad_left = kMaxQualityTilePad;
                    const int pad_top = kMaxQualityTilePad;
                    int pad_right = kMaxQualityTilePad;
                    int pad_bottom = kMaxQualityTilePad;

                    if (((core_in_w + pad_left + pad_right) & 1) != 0) {
                        ++pad_right;
                    }
                    if (((core_in_h + pad_top + pad_bottom) & 1) != 0) {
                        ++pad_bottom;
                    }

                    const int tile_in_w = core_in_w + pad_left + pad_right;
                    const int tile_in_h = core_in_h + pad_top + pad_bottom;
                    const size_t tile_bytes =
                        static_cast<size_t>(tile_in_w) * static_cast<size_t>(tile_in_h) * 3;

                    try {
                        if (tile_rgb.size() < tile_bytes) {
                            tile_rgb.resize(tile_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        if (!trigger_tile_fallback("OOM allocating padded input tile")) {
                            return result;
                        }
                        need_retry = true;
                        break;
                    }

                    fill_padded_tile_rgb(
                        work_ptr,
                        work_is_rgba,
                        plan.work_w,
                        plan.work_h,
                        work_stride,
                        in_x0,
                        in_y0,
                        pad_left,
                        pad_top,
                        tile_in_w,
                        tile_in_h,
                        true, // reflection padding for seamless borders
                        tile_rgb.data()
                    );

                    const TilePassOutcome outcome = run_tile_inference_planar(
                        session,
                        tensor_names,
                        m,
                        tile_rgb.data(),
                        tile_in_w,
                        tile_in_h,
                        config.tta_x8,
                        tta_scratch_rgb,
                        plane_r,
                        plane_g,
                        plane_b,
                        cancel_flag,
                        [&](int) {
                            ++completed_subpasses;
                            const int pct = progress_start +
                                ((progress_end - progress_start) * completed_subpasses) / total_subpasses;
                            report_progress(pct, backend_label);
                        }
                    );

                    if (outcome == TilePassOutcome::CANCELLED) {
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }
                    if (outcome != TilePassOutcome::OK) {
                        if (!trigger_tile_fallback("Tile inference failed")) {
                            return result;
                        }
                        need_retry = true;
                        break;
                    }

                    const int tile_out_w = tile_in_w * m;
                    const int tile_out_h = tile_in_h * m;
                    const int roi_x0 = pad_left * m + (ext_x0 - in_x0 * m);
                    const int roi_y0 = pad_top * m + (band_y0 - in_y0 * m);

                    // Optional adaptive unsharp mask applied inside padded tile BEFORE blending
                    if (config.sharpness > 0) {
                        try {
                            apply_adaptive_unsharp_mask_planar(
                                plane_r.data(),
                                plane_g.data(),
                                plane_b.data(),
                                tile_out_w,
                                tile_out_h,
                                roi_x0,
                                roi_y0,
                                roi_x0 + ext_w,
                                roi_y0 + band_h,
                                config.sharpness
                            );
                        } catch (const std::bad_alloc&) {
                            if (!trigger_tile_fallback("OOM in adaptive unsharp mask")) {
                                return result;
                            }
                            need_retry = true;
                            break;
                        }
                    }

                    // Precompute horizontal linear tent weights across [ext_x0 .. ext_x1)
                    std::vector<float> wx_table(static_cast<size_t>(ext_w));
                    for (int lx = 0; lx < ext_w; ++lx) {
                        wx_table[static_cast<size_t>(lx)] =
                            linear_tent_weight(ext_x0 + lx, sx0, sx1, hl, hr);
                    }

                    // Blend horizontally into `band_accum`
                    #pragma omp parallel for schedule(static)
                    for (int by = 0; by < band_h; ++by) {
                        const size_t src_off =
                            static_cast<size_t>(roi_y0 + by) * static_cast<size_t>(tile_out_w) +
                            static_cast<size_t>(roi_x0);
                        const float* sr = plane_r.data() + src_off;
                        const float* sg = plane_g.data() + src_off;
                        const float* sb = plane_b.data() + src_off;

                        float* drow = band_accum.data() +
                            (static_cast<size_t>(by) * static_cast<size_t>(plan.model_out_w) +
                             static_cast<size_t>(ext_x0)) * 3;

                        for (int lx = 0; lx < ext_w; ++lx) {
                            const float wx = wx_table[static_cast<size_t>(lx)];
                            drow[lx * 3 + 0] += sr[lx] * wx;
                            drow[lx * 3 + 1] += sg[lx] * wx;
                            drow[lx * 3 + 2] += sb[lx] * wx;
                        }
                    }
                }

                if (need_retry) {
                    break;
                }

                // Apply vertical tent weights, merge top 32px overlap with `prev_seam_accum`,
                // emit finalized rows `[band_y0 .. sy1 - vb)`, and save bottom 32px seam for `iy + 1`!
                const size_t row_floats = static_cast<size_t>(plan.model_out_w) * 3;
                const int top_seam_rows = 2 * vt;
                const int bot_seam_rows = 2 * vb;
                const int emit_rows = band_h - bot_seam_rows;

                for (int by = 0; by < band_h; ++by) {
                    const float wy = linear_tent_weight(band_y0 + by, sy0, sy1, vt, vb);
                    float* brow = band_accum.data() + static_cast<size_t>(by) * row_floats;

                    if (by < top_seam_rows) {
                        const float* prev_row = prev_seam_accum.data() + static_cast<size_t>(by) * row_floats;
                        #pragma omp parallel for schedule(static)
                        for (size_t i = 0; i < row_floats; ++i) {
                            brow[i] = prev_row[i] + brow[i] * wy;
                        }
                    } else if (wy != 1.0f) {
                        #pragma omp parallel for schedule(static)
                        for (size_t i = 0; i < row_floats; ++i) {
                            brow[i] *= wy;
                        }
                    }
                }

                if (bot_seam_rows > 0) {
                    const float* bot_src =
                        band_accum.data() + static_cast<size_t>(emit_rows) * row_floats;
                    std::memcpy(
                        prev_seam_accum.data(),
                        bot_src,
                        static_cast<size_t>(bot_seam_rows) * row_floats * sizeof(float)
                    );
                }

                if (emit_rows > 0) {
                    uint8_t* dst_rows_u8 = nullptr;
                    if (!plan.needs_post_resize()) {
                        dst_rows_u8 =
                            final_rgb.get() + static_cast<size_t>(emitted_model_rows) * row_floats;
                    } else {
                        try {
                            const size_t emit_bytes = static_cast<size_t>(emit_rows) * row_floats;
                            if (band_emit_rgb.size() < emit_bytes) {
                                band_emit_rgb.resize(emit_bytes);
                            }
                        } catch (const std::bad_alloc&) {
                            if (!trigger_tile_fallback("OOM allocating streaming emit band")) {
                                return result;
                            }
                            need_retry = true;
                            break;
                        }
                        dst_rows_u8 = band_emit_rgb.data();
                    }

                    const size_t total_emit_vals = static_cast<size_t>(emit_rows) * row_floats;
                    const float* emit_src = band_accum.data();
                    #pragma omp parallel for schedule(static)
                    for (size_t i = 0; i < total_emit_vals; ++i) {
                        dst_rows_u8[i] = clamp_float_255_to_u8(emit_src[i]);
                    }

                    if (plan.needs_post_resize()) {
                        if (!stream_resizer.push_source_rows(dst_rows_u8, emit_rows, cancel_flag)) {
                            if (cancel_flag.load(std::memory_order_relaxed)) {
                                result.status = EnhancerStatus::CANCELLED;
                            } else {
                                result.status = EnhancerStatus::ERR_INFERENCE;
                                result.error_detail = "Streaming Lanczos3 supersampling failed";
                            }
                            return result;
                        }
                    }
                    emitted_model_rows += emit_rows;
                }
            }

            if (!need_retry) {
                inference_done = true;
            }
        } else {
            // =========================================================================
            // STANDARD PATH:
            //   - 10px clamped prepadding + exact core crop
            //   - Supports optional TTA x8 and optional per-tile adaptive unsharp mask
            //   - Band-streamed into `StreamingLanczos3Resizer` when Rule 3 applies
            // =========================================================================
            const int tiles_x = (plan.work_w + active_tile - 1) / active_tile;
            const int tiles_y = (plan.work_h + active_tile - 1) / active_tile;
            const int total_subpasses = std::max(1, tiles_x * tiles_y * passes_per_tile);
            int completed_subpasses = 0;

            for (int ty = 0; ty < tiles_y && !need_retry; ++ty) {
                const int ry0 = ty * active_tile;
                const int rh = std::min(active_tile, plan.work_h - ry0);
                const int core_band_h = rh * m;

                uint8_t* band_base = nullptr;
                if (!plan.needs_post_resize()) {
                    band_base = final_rgb.get() +
                        static_cast<size_t>(ry0 * m) * static_cast<size_t>(plan.model_out_w) * 3;
                } else {
                    try {
                        const size_t band_bytes =
                            static_cast<size_t>(core_band_h) * static_cast<size_t>(plan.model_out_w) * 3;
                        if (band_emit_rgb.size() < band_bytes) {
                            band_emit_rgb.resize(band_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        if (!trigger_tile_fallback("OOM allocating resize band buffer")) {
                            return result;
                        }
                        need_retry = true;
                        break;
                    }
                    band_base = band_emit_rgb.data();
                }

                for (int tx = 0; tx < tiles_x; ++tx) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }

                    const int rx0 = tx * active_tile;
                    const int rw = std::min(active_tile, plan.work_w - rx0);

                    const int pad_left = kStandardTilePad;
                    const int pad_top = kStandardTilePad;
                    int pad_right = kStandardTilePad;
                    int pad_bottom = kStandardTilePad;

                    if (((rw + pad_left + pad_right) & 1) != 0) {
                        ++pad_right;
                    }
                    if (((rh + pad_top + pad_bottom) & 1) != 0) {
                        ++pad_bottom;
                    }

                    const int tile_in_w = rw + pad_left + pad_right;
                    const int tile_in_h = rh + pad_top + pad_bottom;
                    const size_t tile_bytes =
                        static_cast<size_t>(tile_in_w) * static_cast<size_t>(tile_in_h) * 3;

                    try {
                        if (tile_rgb.size() < tile_bytes) {
                            tile_rgb.resize(tile_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        if (!trigger_tile_fallback("OOM allocating tile buffer")) {
                            return result;
                        }
                        need_retry = true;
                        break;
                    }

                    fill_padded_tile_rgb(
                        work_ptr,
                        work_is_rgba,
                        plan.work_w,
                        plan.work_h,
                        work_stride,
                        rx0,
                        ry0,
                        pad_left,
                        pad_top,
                        tile_in_w,
                        tile_in_h,
                        false,
                        tile_rgb.data()
                    );

                    const TilePassOutcome outcome = run_tile_inference_planar(
                        session,
                        tensor_names,
                        m,
                        tile_rgb.data(),
                        tile_in_w,
                        tile_in_h,
                        config.tta_x8,
                        tta_scratch_rgb,
                        plane_r,
                        plane_g,
                        plane_b,
                        cancel_flag,
                        [&](int) {
                            ++completed_subpasses;
                            const int pct = progress_start +
                                ((progress_end - progress_start) * completed_subpasses) / total_subpasses;
                            report_progress(pct, backend_label);
                        }
                    );

                    if (outcome == TilePassOutcome::CANCELLED) {
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }
                    if (outcome != TilePassOutcome::OK) {
                        if (!trigger_tile_fallback("Tile inference failed")) {
                            return result;
                        }
                        need_retry = true;
                        break;
                    }

                    const int tile_out_w = tile_in_w * m;
                    const int tile_out_h = tile_in_h * m;
                    const int crop_x0 = pad_left * m;
                    const int crop_y0 = pad_top * m;
                    const int core_out_w = rw * m;
                    const int core_out_h = rh * m;

                    if (config.sharpness > 0) {
                        try {
                            apply_adaptive_unsharp_mask_planar(
                                plane_r.data(),
                                plane_g.data(),
                                plane_b.data(),
                                tile_out_w,
                                tile_out_h,
                                crop_x0,
                                crop_y0,
                                crop_x0 + core_out_w,
                                crop_y0 + core_out_h,
                                config.sharpness
                            );
                        } catch (const std::bad_alloc&) {
                            if (!trigger_tile_fallback("OOM in adaptive unsharp mask")) {
                                return result;
                            }
                            need_retry = true;
                            break;
                        }
                    }

                    const int dst_x0 = rx0 * m;
                    for (int oy = 0; oy < core_out_h; ++oy) {
                        const size_t src_off =
                            static_cast<size_t>(crop_y0 + oy) * static_cast<size_t>(tile_out_w) +
                            static_cast<size_t>(crop_x0);
                        const float* sr = plane_r.data() + src_off;
                        const float* sg = plane_g.data() + src_off;
                        const float* sb = plane_b.data() + src_off;

                        uint8_t* dst_row = band_base +
                            (static_cast<size_t>(oy) * static_cast<size_t>(plan.model_out_w) +
                             static_cast<size_t>(dst_x0)) * 3;
                        for (int ox = 0; ox < core_out_w; ++ox) {
                            dst_row[ox * 3 + 0] = clamp_float_255_to_u8(sr[ox]);
                            dst_row[ox * 3 + 1] = clamp_float_255_to_u8(sg[ox]);
                            dst_row[ox * 3 + 2] = clamp_float_255_to_u8(sb[ox]);
                        }
                    }
                }

                if (!need_retry && plan.needs_post_resize()) {
                    if (!stream_resizer.push_source_rows(band_base, core_band_h, cancel_flag)) {
                        if (cancel_flag.load(std::memory_order_relaxed)) {
                            result.status = EnhancerStatus::CANCELLED;
                        } else {
                            result.status = EnhancerStatus::ERR_INFERENCE;
                            result.error_detail = "Streaming Lanczos3 upscale failed";
                        }
                        return result;
                    }
                }
            }

            if (!need_retry) {
                inference_done = true;
            }
        }
    }

    // Release Vulkan / ncnn + tile scratch buffers BEFORE JPEG/PNG encoding
    tile_rgb.clear();
    tile_rgb.shrink_to_fit();
    tta_scratch_rgb.clear();
    tta_scratch_rgb.shrink_to_fit();
    plane_r.clear();
    plane_r.shrink_to_fit();
    plane_g.clear();
    plane_g.shrink_to_fit();
    plane_b.clear();
    plane_b.shrink_to_fit();
    band_emit_rgb.clear();
    band_emit_rgb.shrink_to_fit();
    work_rgb_buf.reset();
    session.release();

    if (cancel_flag.load(std::memory_order_relaxed)) {
        result.status = EnhancerStatus::CANCELLED;
        return result;
    }

    // Step 4: Stream-encode packed RGB888 directly to MediaStore file descriptor
    report_progress(95, backend_label);
    std::string encode_err;
    const bool write_ok = write_rgb_image_to_fd(
        owned_output_fd.get(),
        final_rgb.get(),
        plan.final_w,
        plan.final_h,
        config.format,
        config.jpeg_quality,
        &encode_err
    );
    const bool close_ok = owned_output_fd.close_now();
    final_rgb.reset();

    if (!write_ok || !close_ok) {
        result.status = EnhancerStatus::ERR_ENCODE_WRITE;
        result.error_detail = !encode_err.empty() ? encode_err : "Failed closing output file descriptor";
        return result;
    }

    report_progress(100, backend_label);
    result.status = EnhancerStatus::OK;
    return result;
}

} // namespace enhancer
