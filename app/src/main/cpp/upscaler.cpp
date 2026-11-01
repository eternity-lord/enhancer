#include "upscaler.h"

#include "image_io.h"
#include "resizer.h"
#include "tile_ops.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// Prebuilt ncnn headers (arm64-v8a with Vulkan)
#include "allocator.h"
#include "cpu.h"
#include "gpu.h"
#include "mat.h"
#include "net.h"

#define LOG_TAG "EnhancerNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace enhancer {

namespace {

constexpr int kDefaultTileSize = 200;
constexpr int kMinTileSize = 48;
constexpr int kSafeModeTileSize = 64;
constexpr int kStandardTilePad = 10;
constexpr int kMaxQualityTilePad = 32;
constexpr int kMaxQualityFeatherOut = 32;
constexpr int kMaxQualityHalfFeather = kMaxQualityFeatherOut / 2; // 16 output pixels each side

// Balanced process-lifetime Vulkan instance manager:
// `create_gpu_instance()` is called lazily at most once, and `destroy_gpu_instance()` is called
// once on `shutdown_vulkan_runtime()` (`JNI_OnUnload`) iff creation succeeded (`rc == 0`).
std::mutex g_vulkan_mutex;
bool g_vulkan_init_attempted = false;
bool g_vulkan_instance_created = false;
int g_vulkan_create_rc = 0;

bool ensure_vulkan_ready(int* out_create_rc = nullptr) noexcept {
    std::lock_guard<std::mutex> lock(g_vulkan_mutex);
    if (!g_vulkan_init_attempted) {
        g_vulkan_init_attempted = true;
        g_vulkan_create_rc = ncnn::create_gpu_instance();
        g_vulkan_instance_created = (g_vulkan_create_rc == 0);
        if (!g_vulkan_instance_created) {
            LOGW("ncnn::create_gpu_instance() returned rc=%d; Vulkan unavailable", g_vulkan_create_rc);
        } else {
            LOGI("ncnn::create_gpu_instance() succeeded (gpu_count=%d)", ncnn::get_gpu_count());
        }
    }
    if (out_create_rc) {
        *out_create_rc = g_vulkan_create_rc;
    }
    if (!g_vulkan_instance_created) {
        return false;
    }
    return ncnn::get_gpu_count() > 0 && ncnn::get_default_gpu_index() >= 0;
}

uint64_t query_asset_size_bytes(AAssetManager* mgr, const std::string& asset_path) noexcept {
    if (!mgr || asset_path.empty()) {
        return 0;
    }
    AAsset* asset = AAssetManager_open(mgr, asset_path.c_str(), AASSET_MODE_UNKNOWN);
    if (!asset) {
        return 0;
    }
    const off64_t len = AAsset_getLength64(asset);
    AAsset_close(asset);
    return (len > 0) ? static_cast<uint64_t>(len) : 0;
}

// Queries `vkdev->get_heap_budget()` (MB) and caps initial tile size on low-VRAM GPUs.
int choose_tile_for_gpu_budget(
    int requested_tile,
    uint32_t heap_budget_mb,
    bool max_quality,
    bool tta_x8
) noexcept {
    int tile = std::max(kMinTileSize, requested_tile);
    if (heap_budget_mb == 0) {
        return tile;
    }
    if (heap_budget_mb < 160u) {
        tile = std::min(tile, 64);
    } else if (heap_budget_mb < 300u) {
        tile = std::min(tile, 96);
    } else if (heap_budget_mb < 512u) {
        tile = std::min(tile, 128);
    } else if (heap_budget_mb < 900u) {
        tile = std::min(tile, 160);
    }
    if ((max_quality || tta_x8) && heap_budget_mb < 650u) {
        tile = std::min(tile, 128);
    }
    return std::max(kMinTileSize, tile);
}

struct FailureInfo {
    EnhancerStatus status = EnhancerStatus::OK;
    std::string step;
    uint64_t requested_bytes = 0;
    int tile_size = 0;
    std::string backend;
    int ncnn_rc = 0;
    std::string extra;

    std::string format() const {
        return format_step_diagnostic(
            status,
            step,
            requested_bytes,
            tile_size,
            backend,
            ncnn_rc,
            extra
        );
    }
};

// Manages a single model load per job and dedicated per-tile Vulkan blob/staging allocators.
// Static weights are loaded BEFORE acquiring the per-tile `blob_vkallocator_` so calling
// `clear_tile_allocators()` after every tile strictly flushes transient tile activations/staging
// without touching static network weights.
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
        int current_tile_size,
        std::string& out_backend_label,
        uint32_t& out_heap_budget_mb,
        FailureInfo& out_failure
    ) {
        release();
        use_vulkan_ = use_vulkan;
        out_heap_budget_mb = 0;

        const int big_cores = ncnn::get_big_cpu_count();
        const int all_cores = ncnn::get_cpu_count();
        const int threads = std::clamp(big_cores > 0 ? big_cores : all_cores, 1, 8);

        net_.opt = ncnn::Option();
        net_.opt.num_threads = threads;
        net_.opt.lightmode = true;
        net_.opt.use_int8_packed = false;
        net_.opt.use_int8_storage = false;
        net_.opt.use_int8_arithmetic = false;

        if (config.disable_fp16) {
            net_.opt.use_fp16_packed = false;
            net_.opt.use_fp16_storage = false;
            net_.opt.use_fp16_arithmetic = false;
        } else {
            net_.opt.use_fp16_packed = true;
            net_.opt.use_fp16_storage = true;
            // Keep fp16_arithmetic = false to prevent deep RRDB FP16 overflow / NaN on mobile GPUs
            net_.opt.use_fp16_arithmetic = false;
        }

        if (use_vulkan_) {
            int create_rc = 0;
            if (!ensure_vulkan_ready(&create_rc)) {
                out_backend_label = "Vulkan";
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_VULKAN_INIT,
                    "ncnn::create_gpu_instance / get_gpu_count",
                    0,
                    current_tile_size,
                    out_backend_label,
                    create_rc,
                    "gpu_count=" + std::to_string(g_vulkan_instance_created ? ncnn::get_gpu_count() : 0)
                };
                return false;
            }

            const int gpu_idx = ncnn::get_default_gpu_index();
            vkdev_ = (gpu_idx >= 0) ? ncnn::get_gpu_device(gpu_idx) : nullptr;
            if (!vkdev_ || !vkdev_->is_valid()) {
                out_backend_label = "Vulkan";
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_VULKAN_INIT,
                    "ncnn::get_gpu_device(" + std::to_string(gpu_idx) + ")",
                    0,
                    current_tile_size,
                    out_backend_label,
                    -1,
                    "VulkanDevice null or invalid"
                };
                vkdev_ = nullptr;
                return false;
            }

            const char* dev_name = vkdev_->info.device_name();
            out_backend_label = std::string("Vulkan (") +
                ((dev_name && dev_name[0] != '\0') ? dev_name : "GPU") + ")";
            out_heap_budget_mb = vkdev_->get_heap_budget();

            net_.opt.use_vulkan_compute = true;
            net_.opt.vulkan_device_index = gpu_idx;
            net_.set_vulkan_device(vkdev_);
        } else {
            net_.opt.use_vulkan_compute = false;
            out_backend_label = "CPU (" + std::to_string(threads) + " threads)";
        }

        const uint64_t param_bytes = query_asset_size_bytes(mgr, config.param_asset_path);
        const int param_rc = net_.load_param(mgr, config.param_asset_path.c_str());
        if (param_rc != 0) {
            out_failure = FailureInfo{
                EnhancerStatus::ERR_NCNN_LOAD_PARAM,
                "ncnn::Net::load_param(" + config.param_asset_path + ")",
                param_bytes,
                current_tile_size,
                out_backend_label,
                param_rc,
                ""
            };
            release();
            return false;
        }

        const uint64_t model_bytes = query_asset_size_bytes(mgr, config.bin_asset_path);
        const int model_rc = net_.load_model(mgr, config.bin_asset_path.c_str());
        if (model_rc != 0) {
            out_failure = FailureInfo{
                EnhancerStatus::ERR_NCNN_LOAD_MODEL,
                "ncnn::Net::load_model(" + config.bin_asset_path + ")",
                model_bytes,
                current_tile_size,
                out_backend_label,
                model_rc,
                "heap_budget_mb=" + std::to_string(out_heap_budget_mb)
            };
            release();
            return false;
        }

        // Acquire per-tile transient Vulkan blob & staging allocators AFTER static model weights are loaded.
        if (use_vulkan_ && vkdev_) {
            blob_vkallocator_ = vkdev_->acquire_blob_allocator();
            staging_vkallocator_ = vkdev_->acquire_staging_allocator();
            if (!blob_vkallocator_ || !staging_vkallocator_) {
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_VK_ALLOCATOR,
                    "VulkanDevice::acquire_blob_allocator / acquire_staging_allocator",
                    0,
                    current_tile_size,
                    out_backend_label,
                    -1,
                    "blob=" + std::string(blob_vkallocator_ ? "ok" : "null") +
                        ", staging=" + std::string(staging_vkallocator_ ? "ok" : "null") +
                        ", heap_budget_mb=" + std::to_string(out_heap_budget_mb)
                };
                release();
                return false;
            }
        }

        // Resolve exact input/output blob names directly from the loaded ncnn::Net graph
        // (all 5 bundled Real-ESRGAN `.param` files declare `"data"` and `"output"`, and reading
        // `net_.input_names()` / `net_.output_names()` prevents any hardcoded mismatch).
        const std::vector<const char*>& in_names = net_.input_names();
        const std::vector<const char*>& out_names = net_.output_names();
        input_blob_ = (!in_names.empty() && in_names[0] != nullptr && in_names[0][0] != '\0')
            ? std::string(in_names[0])
            : std::string("data");
        output_blob_ = (!out_names.empty() && out_names[0] != nullptr && out_names[0][0] != '\0')
            ? std::string(out_names[0])
            : std::string("output");

        LOGI(
            "Model loaded once: param=%s bin=%s (%llu B) backend=%s heap_budget=%u MB blobs=[%s -> %s]",
            config.param_asset_path.c_str(),
            config.bin_asset_path.c_str(),
            static_cast<unsigned long long>(model_bytes),
            out_backend_label.c_str(),
            out_heap_budget_mb,
            input_blob_.c_str(),
            output_blob_.c_str()
        );
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
        // Clear Net first so no buffers reference VkAllocators or VulkanDevice
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
        use_vulkan_ = false;
    }

    ncnn::Net& net() noexcept { return net_; }
    bool using_vulkan() const noexcept { return use_vulkan_; }
    ncnn::VkAllocator* blob_vkallocator() const noexcept { return blob_vkallocator_; }
    ncnn::VkAllocator* staging_vkallocator() const noexcept { return staging_vkallocator_; }
    const char* input_blob() const noexcept { return input_blob_.c_str(); }
    const char* output_blob() const noexcept { return output_blob_.c_str(); }

private:
    ncnn::Net net_;
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::VkAllocator* blob_vkallocator_ = nullptr;
    ncnn::VkAllocator* staging_vkallocator_ = nullptr;
    bool use_vulkan_ = false;
    std::string input_blob_ = "data";
    std::string output_blob_ = "output";
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

inline uint16_t clamp_float_255_weighted_q8(float v, uint32_t weight_q8) noexcept {
    if (!(v > 0.0f) || weight_q8 == 0u) {
        return 0u;
    }
    const float clamped = (v >= 255.0f) ? 255.0f : v;
    const float scaled = clamped * static_cast<float>(weight_q8) + 0.5f;
    if (scaled >= 65280.0f) {
        return 65280u;
    }
    return static_cast<uint16_t>(scaled);
}

// Quantizes linear tent weight on [0..256] such that left + right weights across any 32px
// interior feather seam sum to EXACTLY 256 (exact integer partition of unity).
inline uint32_t linear_tent_weight_q8(
    int p,
    int seg0,
    int seg1,
    int half_left,
    int half_right
) noexcept {
    if (half_left > 0 && p < seg0 + half_left) {
        const int idx = p - (seg0 - half_left); // 0 .. 2*half_left - 1
        const int width = 2 * half_left;
        // Symmetric rounding so w_left(width - 1 - idx) + w_right(idx) == 256
        return static_cast<uint32_t>(((2 * idx + 1) * 256 + width) / (2 * width));
    }
    if (half_right > 0 && p >= seg1 - half_right) {
        const int idx = p - (seg1 - half_right); // 0 .. 2*half_right - 1
        const int width = 2 * half_right;
        const uint32_t rising = static_cast<uint32_t>(((2 * idx + 1) * 256 + width) / (2 * width));
        return 256u - rising;
    }
    return 256u;
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
    size_t work_stride_bytes,
    int core_x0,
    int core_y0,
    int pad_left,
    int pad_top,
    int tile_in_w,
    int tile_in_h,
    bool reflect_pad,
    uint8_t* dst_tile_rgb
) {
    const size_t dst_row_bytes = static_cast<size_t>(tile_in_w) * 3u;
    for (int ty = 0; ty < tile_in_h; ++ty) {
        const int raw_sy = core_y0 - pad_top + ty;
        const int sy = reflect_pad
            ? reflect_coord(raw_sy, work_h)
            : std::clamp(raw_sy, 0, work_h - 1);
        const uint8_t* src_row =
            work_rgba_or_rgb + static_cast<size_t>(sy) * work_stride_bytes;
        uint8_t* dst_row = dst_tile_rgb + static_cast<size_t>(ty) * dst_row_bytes;

        if (work_is_rgba) {
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int raw_sx = core_x0 - pad_left + tx;
                const int sx = reflect_pad
                    ? reflect_coord(raw_sx, work_w)
                    : std::clamp(raw_sx, 0, work_w - 1);
                const uint8_t* spx = src_row + static_cast<size_t>(sx) * 4u;
                const size_t dx3 = static_cast<size_t>(tx) * 3u;
                dst_row[dx3 + 0] = spx[0];
                dst_row[dx3 + 1] = spx[1];
                dst_row[dx3 + 2] = spx[2];
            }
        } else {
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int raw_sx = core_x0 - pad_left + tx;
                const int sx = reflect_pad
                    ? reflect_coord(raw_sx, work_w)
                    : std::clamp(raw_sx, 0, work_w - 1);
                const uint8_t* spx = src_row + static_cast<size_t>(sx) * 3u;
                const size_t dx3 = static_cast<size_t>(tx) * 3u;
                dst_row[dx3 + 0] = spx[0];
                dst_row[dx3 + 1] = spx[1];
                dst_row[dx3 + 2] = spx[2];
            }
        }
    }
}

enum class TilePassOutcome {
    OK = 0,
    CANCELLED = 1,
    FAILED = 2,
};

// Runs 1-pass or 8-pass TTA inference on a single prepared `tile_in_w x tile_in_h` RGB888 tile,
// producing per-tile planar float RGB (`tile_out_w x tile_out_h`) scaled to [0..255].
// Calls `blob_vkallocator->clear()` and `staging_vkallocator->clear()` AFTER `Extractor` and `Mat out`
// have been destroyed at the end of EVERY tile pass.
TilePassOutcome run_tile_inference_planar(
    ScopedModelSession& session,
    int model_scale,
    int nominal_tile_size,
    const std::string& backend_label,
    const uint8_t* base_tile_rgb,
    int tile_in_w,
    int tile_in_h,
    bool tta_x8,
    std::vector<uint8_t>& tta_scratch_rgb,
    std::vector<float>& out_plane_r,
    std::vector<float>& out_plane_g,
    std::vector<float>& out_plane_b,
    FailureInfo& out_failure,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pass_done_0_based)>& on_subpass_done
) {
    const int64_t out_w64 = static_cast<int64_t>(tile_in_w) * static_cast<int64_t>(model_scale);
    const int64_t out_h64 = static_cast<int64_t>(tile_in_h) * static_cast<int64_t>(model_scale);
    if (out_w64 <= 0 || out_h64 <= 0 ||
        out_w64 > std::numeric_limits<int>::max() ||
        out_h64 > std::numeric_limits<int>::max()) {
        out_failure = FailureInfo{
            EnhancerStatus::ERR_INVALID_ARGS,
            "tile_output_dimensions",
            0,
            nominal_tile_size,
            backend_label,
            0,
            "overflow computing tile_out_w/h"
        };
        return TilePassOutcome::FAILED;
    }

    const int tile_out_w = static_cast<int>(out_w64);
    const int tile_out_h = static_cast<int>(out_h64);
    size_t out_pixels = 0;
    size_t plane_total_bytes = 0;
    if (!checked_mul_size(static_cast<size_t>(tile_out_w), static_cast<size_t>(tile_out_h), out_pixels) ||
        !checked_mul_size(out_pixels, 3u * sizeof(float), plane_total_bytes)) {
        out_failure = FailureInfo{
            EnhancerStatus::ERR_INVALID_ARGS,
            "tile_planar_float_size_overflow",
            0,
            nominal_tile_size,
            backend_label,
            0,
            ""
        };
        return TilePassOutcome::FAILED;
    }

    try {
        out_plane_r.assign(out_pixels, 0.0f);
        out_plane_g.assign(out_pixels, 0.0f);
        out_plane_b.assign(out_pixels, 0.0f);
    } catch (const std::bad_alloc&) {
        out_failure = FailureInfo{
            EnhancerStatus::ERR_OUT_OF_MEMORY,
            "alloc_per_tile_float_planes",
            static_cast<uint64_t>(plane_total_bytes),
            nominal_tile_size,
            backend_label,
            0,
            "tile_out=" + std::to_string(tile_out_w) + "x" + std::to_string(tile_out_h)
        };
        return TilePassOutcome::FAILED;
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
            size_t tta_bytes = 0;
            checked_image_bytes(tile_in_w, tile_in_h, 3, tta_bytes);
            try {
                d4_transform_rgb(base_tile_rgb, tile_in_w, tile_in_h, t, tta_scratch_rgb, pass_in_w, pass_in_h);
            } catch (const std::bad_alloc&) {
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_OUT_OF_MEMORY,
                    "alloc_tta_tile_scratch",
                    static_cast<uint64_t>(tta_bytes),
                    nominal_tile_size,
                    backend_label,
                    0,
                    ""
                };
                return TilePassOutcome::FAILED;
            }
            pass_rgb = tta_scratch_rgb.data();
        }

        size_t in_mat_bytes = 0;
        checked_mul3_size(
            static_cast<size_t>(pass_in_w),
            static_cast<size_t>(pass_in_h),
            3u * sizeof(float),
            in_mat_bytes
        );

        // Inner scope ensures `ncnn::Mat in`, `ncnn::Extractor ex`, and `ncnn::Mat out`
        // are ALL completely destroyed BEFORE `session.clear_tile_allocators()` runs!
        bool pass_failed = false;
        {
            ncnn::Mat in = ncnn::Mat::from_pixels(
                pass_rgb,
                ncnn::Mat::PIXEL_RGB,
                pass_in_w,
                pass_in_h
            );
            if (in.empty()) {
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_NCNN_INPUT,
                    "ncnn::Mat::from_pixels",
                    static_cast<uint64_t>(in_mat_bytes),
                    nominal_tile_size,
                    backend_label,
                    -1,
                    "padded_tile=" + std::to_string(pass_in_w) + "x" + std::to_string(pass_in_h)
                };
                pass_failed = true;
            } else {
                in.substract_mean_normalize(nullptr, kNormVals);

                ncnn::Extractor ex = session.net().create_extractor();
                ex.set_light_mode(true);
                if (session.using_vulkan()) {
                    if (session.blob_vkallocator()) {
                        ex.set_blob_vkallocator(session.blob_vkallocator());
                        ex.set_workspace_vkallocator(session.blob_vkallocator());
                    }
                    if (session.staging_vkallocator()) {
                        ex.set_staging_vkallocator(session.staging_vkallocator());
                    }
                }

                const int in_rc = ex.input(session.input_blob(), in);
                if (in_rc != 0) {
                    out_failure = FailureInfo{
                        EnhancerStatus::ERR_NCNN_INPUT,
                        std::string("ncnn::Extractor::input(blob=\"") + session.input_blob() + "\")",
                        static_cast<uint64_t>(in_mat_bytes),
                        nominal_tile_size,
                        backend_label,
                        in_rc,
                        "padded_tile=" + std::to_string(pass_in_w) + "x" + std::to_string(pass_in_h)
                    };
                    pass_failed = true;
                } else {
                    const int expected_w = pass_in_w * model_scale;
                    const int expected_h = pass_in_h * model_scale;
                    size_t expected_out_bytes = 0;
                    checked_mul3_size(
                        static_cast<size_t>(expected_w),
                        static_cast<size_t>(expected_h),
                        3u * sizeof(float),
                        expected_out_bytes
                    );

                    ncnn::Mat out;
                    const int ex_rc = ex.extract(session.output_blob(), out);
                    if (ex_rc != 0 || out.empty()) {
                        out_failure = FailureInfo{
                            EnhancerStatus::ERR_NCNN_EXTRACT,
                            std::string("ncnn::Extractor::extract(blob=\"") + session.output_blob() + "\")",
                            static_cast<uint64_t>(expected_out_bytes),
                            nominal_tile_size,
                            backend_label,
                            ex_rc,
                            "out_empty=" + std::string(out.empty() ? "true" : "false") +
                                ", padded_tile=" + std::to_string(pass_in_w) + "x" + std::to_string(pass_in_h)
                        };
                        pass_failed = true;
                    } else if (out.c < 3 || out.w != expected_w || out.h != expected_h) {
                        out_failure = FailureInfo{
                            EnhancerStatus::ERR_INFERENCE,
                            "validate_output_mat_shape",
                            static_cast<uint64_t>(expected_out_bytes),
                            nominal_tile_size,
                            backend_label,
                            0,
                            "got=" + std::to_string(out.w) + "x" + std::to_string(out.h) + "x" + std::to_string(out.c) +
                                ", expected=" + std::to_string(expected_w) + "x" + std::to_string(expected_h) + "x3"
                        };
                        pass_failed = true;
                    } else {
                        const ncnn::Mat ch_r_mat = out.channel(0);
                        const ncnn::Mat ch_g_mat = out.channel(1);
                        const ncnn::Mat ch_b_mat = out.channel(2);
                        const float* ch_r = ch_r_mat;
                        const float* ch_g = ch_g_mat;
                        const float* ch_b = ch_b_mat;

                        // Guard against mobile GPU Vulkan driver NaN/Inf shader output
                        if (session.using_vulkan()) {
                            const size_t total_px = static_cast<size_t>(expected_w) * static_cast<size_t>(expected_h);
                            const size_t samples[3] = {0u, total_px / 2u, total_px - 1u};
                            for (size_t sidx : samples) {
                                if (!std::isfinite(ch_r[sidx]) || !std::isfinite(ch_g[sidx]) || !std::isfinite(ch_b[sidx])) {
                                    out_failure = FailureInfo{
                                        EnhancerStatus::ERR_INFERENCE,
                                        "vulkan_nan_or_inf_output",
                                        static_cast<uint64_t>(expected_out_bytes),
                                        nominal_tile_size,
                                        backend_label,
                                        0,
                                        "non-finite float detected in Vulkan output tile"
                                    };
                                    pass_failed = true;
                                    break;
                                }
                            }
                        }

                        if (!pass_failed) {
                            d4_inverse_accumulate_planar(
                                ch_r,
                                ch_g,
                                ch_b,
                                static_cast<size_t>(out.w),
                                expected_w,
                                expected_h,
                                t,
                                scale_255,
                                out_plane_r.data(),
                                out_plane_g.data(),
                                out_plane_b.data(),
                                tile_out_w,
                                tile_out_h
                            );
                        }
                    }
                }
            }
        }

        // `Extractor ex` and `Mat out` are now destroyed; flush per-tile Vulkan pools immediately!
        session.clear_tile_allocators();
        if (pass_failed) {
            return TilePassOutcome::FAILED;
        }

        if (on_subpass_done) {
            on_subpass_done(t);
        }
    }

    return TilePassOutcome::OK;
}

// Shared core execution supporting both MediaStore file output (`write_to_fd = true`)
// and in-memory synthetic self-test (`write_to_fd = false`, `allow_fallbacks = false`).
UpscaleResult execute_pipeline_internal(
    AAssetManager* asset_manager,
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    const std::function<void()>& unlock_bitmap_fn,
    const UpscaleConfig& raw_config,
    bool write_to_fd,
    bool allow_fallbacks,
    uint8_t* out_sample_stats_min_max, // optional 2-byte [min, max] check for self-test
    const std::atomic<bool>& cancel_flag,
    const ProgressCallback& progress_cb
) {
    UpscaleResult result;
    UniqueFd owned_output_fd(write_to_fd ? raw_config.output_fd : -1);

    bool bitmap_unlocked = false;
    auto unlock_source_once = [&]() noexcept {
        if (!bitmap_unlocked) {
            bitmap_unlocked = true;
            if (unlock_bitmap_fn) {
                unlock_bitmap_fn();
            }
        }
    };

    UpscaleConfig config = raw_config;
    if (config.safe_mode) {
        // Safe mode forces: CPU + tile 64 + no Max quality + no TTA
        config.force_cpu = true;
        config.requested_tile_size = kSafeModeTileSize;
        config.max_quality = false;
        config.tta_x8 = false;
    }

    const int requested_base_tile = (config.requested_tile_size > 0)
        ? std::max(kMinTileSize, config.requested_tile_size)
        : kDefaultTileSize;

    if ((write_to_fd && !owned_output_fd.valid()) ||
        !asset_manager || !src_rgba ||
        src_w <= 0 || src_h <= 0 ||
        static_cast<int64_t>(src_stride_bytes) < static_cast<int64_t>(src_w) * 4 ||
        (config.model_scale != 2 && config.model_scale != 3 && config.model_scale != 4)) {
        unlock_source_once();
        result.status = EnhancerStatus::ERR_INVALID_ARGS;
        result.error_detail = format_step_diagnostic(
            EnhancerStatus::ERR_INVALID_ARGS,
            "validate_pipeline_args",
            0,
            requested_base_tile,
            config.force_cpu ? "CPU" : "Vulkan",
            0,
            "src=" + std::to_string(src_w) + "x" + std::to_string(src_h) +
                ", scale=" + std::to_string(config.model_scale)
        );
        return result;
    }

    if (cancel_flag.load(std::memory_order_relaxed)) {
        unlock_source_once();
        result.status = EnhancerStatus::CANCELLED;
        return result;
    }

    // Active runtime state (can step down via exact fallback chain on real failure)
    bool active_max_quality = config.max_quality;
    bool active_tta = config.tta_x8;
    bool active_vulkan = !config.force_cpu;
    int active_tile = requested_base_tile;

    std::vector<std::string> fallback_steps;
    auto record_fallback = [&](const std::string& step_desc) {
        fallback_steps.push_back(step_desc);
        LOGW("Automatic fallback triggered: %s", step_desc.c_str());
    };
    auto build_fallback_summary = [&]() -> std::string {
        std::string joined;
        for (size_t i = 0; i < fallback_steps.size(); ++i) {
            if (i > 0) {
                joined += " -> ";
            }
            joined += fallback_steps[i];
        }
        return joined;
    };

    const auto report_progress = [&](int pct, const std::string& backend_str) {
        if (progress_cb) {
            progress_cb(std::clamp(pct, 0, 100), backend_str, active_tile);
        }
    };

    // Initialize model session ONCE per job (re-initialized ONLY if fallback switches Vulkan -> CPU)
    ScopedModelSession session;
    std::string backend_label;
    uint32_t gpu_heap_budget_mb = 0;
    FailureInfo init_failure;

    if (active_vulkan) {
        if (session.init(
                asset_manager,
                config,
                true,
                active_tile,
                backend_label,
                gpu_heap_budget_mb,
                init_failure
            )) {
            // Apply GPU heap budget cap when user left tile on Auto (<=0) or requested a large tile on low VRAM
            const int budget_tile = choose_tile_for_gpu_budget(
                active_tile,
                gpu_heap_budget_mb,
                active_max_quality,
                active_tta
            );
            if (budget_tile < active_tile) {
                LOGI(
                    "Capping initial tile %d -> %d based on Vulkan heap budget (%u MB)",
                    active_tile,
                    budget_tile,
                    gpu_heap_budget_mb
                );
                active_tile = budget_tile;
            }
        } else {
            if (!allow_fallbacks) {
                unlock_source_once();
                result.status = init_failure.status;
                result.backend_used = backend_label;
                result.final_tile_size = active_tile;
                result.error_detail = init_failure.format();
                return result;
            }
            LOGW("Vulkan init failed (%s); switching to CPU backend", init_failure.format().c_str());
            active_vulkan = false;
            result.used_cpu_fallback = true;
            record_fallback("switched to CPU backend (Vulkan init failed)");
        }
    }

    if (!active_vulkan) {
        FailureInfo cpu_init_failure;
        if (!session.init(
                asset_manager,
                config,
                false,
                active_tile,
                backend_label,
                gpu_heap_budget_mb,
                cpu_init_failure
            )) {
            unlock_source_once();
            result.status = cpu_init_failure.status;
            result.backend_used = backend_label;
            result.final_tile_size = active_tile;
            result.fallback_summary = build_fallback_summary();
            result.error_detail = cpu_init_failure.format();
            return result;
        }
    }

    result.backend_used = backend_label;
    result.final_tile_size = active_tile;
    report_progress(3, backend_label);

    // Buffers managed across retries:
    // INVARIANT: At most ONE full-size output buffer (`final_rgb`) ever exists at any instant!
    DimensionPlan plan;
    bool plan_prepared = false;
    bool prepared_for_max_quality = false;

    NativeByteBuffer work_rgb_buf;
    const uint8_t* work_ptr = src_rgba;
    bool work_is_rgba = true;
    size_t work_stride = static_cast<size_t>(src_stride_bytes);

    NativeByteBuffer final_rgb;
    StreamingLanczos3Resizer stream_resizer;

    auto prepare_buffers_for_mode = [&](FailureInfo& prep_fail) -> bool {
        if (plan_prepared && prepared_for_max_quality == active_max_quality && final_rgb) {
            if (plan.needs_post_resize()) {
                stream_resizer.reset_stream();
            }
            return true;
        }

        // Release any previous `final_rgb` FIRST so TWO full-size buffers NEVER coexist!
        final_rgb.reset();
        stream_resizer = StreamingLanczos3Resizer{};

        plan = compute_dimensions(
            src_w,
            src_h,
            config.model_scale,
            config.target_long_side,
            active_max_quality
        );
        if (plan.final_w <= 0 || plan.final_h <= 0 || plan.work_w <= 0 || plan.work_h <= 0) {
            prep_fail = FailureInfo{
                EnhancerStatus::ERR_INVALID_ARGS,
                "compute_dimensions",
                0,
                active_tile,
                backend_label,
                0,
                "src=" + std::to_string(src_w) + "x" + std::to_string(src_h) +
                    ", target=" + std::to_string(config.target_long_side)
            };
            return false;
        }

        result.out_width = plan.final_w;
        result.out_height = plan.final_h;

        // Pre-downscale source into compact `work_rgb_buf` when Standard Rule 2 applies
        if (plan.pre_downscaled && !work_rgb_buf) {
            size_t work_bytes = 0;
            if (!checked_image_bytes(plan.work_w, plan.work_h, 3, work_bytes)) {
                prep_fail = FailureInfo{
                    EnhancerStatus::ERR_INVALID_ARGS,
                    "checked_image_bytes(work_rgb_buf)",
                    0,
                    active_tile,
                    backend_label,
                    0,
                    ""
                };
                return false;
            }

            report_progress(4, backend_label);
            work_rgb_buf = allocate_native_buffer(work_bytes);
            if (!work_rgb_buf) {
                prep_fail = FailureInfo{
                    EnhancerStatus::ERR_OUT_OF_MEMORY,
                    "malloc(work_rgb_buf)",
                    static_cast<uint64_t>(work_bytes),
                    active_tile,
                    backend_label,
                    0,
                    "work=" + std::to_string(plan.work_w) + "x" + std::to_string(plan.work_h)
                };
                return false;
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
                work_rgb_buf.reset();
                if (cancel_flag.load(std::memory_order_relaxed)) {
                    prep_fail.status = EnhancerStatus::CANCELLED;
                } else {
                    prep_fail = FailureInfo{
                        EnhancerStatus::ERR_INFERENCE,
                        "downscale_rgba_to_rgb_box",
                        static_cast<uint64_t>(work_bytes),
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                }
                return false;
            }

            work_ptr = work_rgb_buf.get();
            work_is_rgba = false;
            work_stride = static_cast<size_t>(plan.work_w) * 3u;
            // Source bitmap is no longer needed once pre-downscaled; free it BEFORE allocating `final_rgb`!
            unlock_source_once();
        }

        // Allocate the SINGLE full-size output RGB888 buffer (`final_w * final_h * 3` bytes)
        size_t final_bytes = 0;
        if (!checked_image_bytes(plan.final_w, plan.final_h, 3, final_bytes)) {
            prep_fail = FailureInfo{
                EnhancerStatus::ERR_INVALID_ARGS,
                "checked_image_bytes(final_rgb)",
                0,
                active_tile,
                backend_label,
                0,
                "final=" + std::to_string(plan.final_w) + "x" + std::to_string(plan.final_h)
            };
            return false;
        }

        final_rgb = allocate_native_buffer(final_bytes);
        if (!final_rgb) {
            prep_fail = FailureInfo{
                EnhancerStatus::ERR_OUT_OF_MEMORY,
                "malloc(final_rgb)",
                static_cast<uint64_t>(final_bytes),
                active_tile,
                backend_label,
                0,
                "final=" + std::to_string(plan.final_w) + "x" + std::to_string(plan.final_h) +
                    ", target=" + std::to_string(config.target_long_side)
            };
            return false;
        }

        if (plan.needs_post_resize()) {
            const uint64_t approx_ring_bytes =
                static_cast<uint64_t>(plan.final_w) * 3u * sizeof(float) * 32u;
            try {
                if (!stream_resizer.init(
                        plan.model_out_w,
                        plan.model_out_h,
                        final_rgb.get(),
                        plan.final_w,
                        plan.final_h
                    )) {
                    prep_fail = FailureInfo{
                        EnhancerStatus::ERR_INFERENCE,
                        "StreamingLanczos3Resizer::init",
                        approx_ring_bytes,
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                    return false;
                }
            } catch (const std::bad_alloc&) {
                prep_fail = FailureInfo{
                    EnhancerStatus::ERR_OUT_OF_MEMORY,
                    "std::bad_alloc(StreamingLanczos3Resizer::init)",
                    approx_ring_bytes,
                    active_tile,
                    backend_label,
                    0,
                    ""
                };
                return false;
            }
        }

        plan_prepared = true;
        prepared_for_max_quality = active_max_quality;
        return true;
    };

    // Exact ordered fallback state machine requested by specification:
    //   1) halve tile (down to 48)
    //   2) disable TTA
    //   3) disable Max quality
    //   4) switch to CPU backend
    auto advance_fallback_chain = [&](const FailureInfo& cause, FailureInfo& terminal_out) -> bool {
        session.clear_tile_allocators();
        if (!allow_fallbacks) {
            terminal_out = cause;
            return false;
        }

        // Non-recoverable invalid arguments should not spin through fallbacks
        if (cause.status == EnhancerStatus::ERR_INVALID_ARGS) {
            terminal_out = cause;
            return false;
        }

        if (active_tile > kMinTileSize) {
            const int prev_tile = active_tile;
            active_tile = std::max(kMinTileSize, active_tile / 2);
            result.final_tile_size = active_tile;
            record_fallback(
                "halved tile " + std::to_string(prev_tile) + "->" + std::to_string(active_tile)
            );
            return true;
        }

        if (active_tta) {
            active_tta = false;
            record_fallback("disabled TTA");
            return true;
        }

        if (active_max_quality) {
            active_max_quality = false;
            plan_prepared = false; // triggers `prepare_buffers_for_mode` for Standard mode
            record_fallback("disabled Max quality");
            return true;
        }

        if (active_vulkan) {
            active_vulkan = false;
            result.used_cpu_fallback = true;
            // Release Vulkan session + VkAllocators completely before initializing CPU session
            session.release();
            active_tile = std::max(kMinTileSize, std::min(requested_base_tile, 128));
            result.final_tile_size = active_tile;
            record_fallback("switched to CPU backend");

            uint32_t unused_budget = 0;
            FailureInfo cpu_err;
            if (!session.init(
                    asset_manager,
                    config,
                    false,
                    active_tile,
                    backend_label,
                    unused_budget,
                    cpu_err
                )) {
                terminal_out = cpu_err;
                terminal_out.extra += " | after_fallbacks=" + build_fallback_summary();
                return false;
            }
            result.backend_used = backend_label;
            return true;
        }

        terminal_out = cause;
        if (!fallback_steps.empty()) {
            terminal_out.extra += (terminal_out.extra.empty() ? "" : "; ") +
                std::string("exhausted_fallbacks=[") + build_fallback_summary() + "]";
        }
        return false;
    };

    const int m = config.model_scale;
    const int progress_start = 5;
    const int progress_end = 92;

    // Per-tile scratch buffers (NEVER full-size!)
    std::vector<uint8_t> tile_rgb;
    std::vector<uint8_t> tta_scratch_rgb;
    std::vector<float> plane_r;
    std::vector<float> plane_g;
    std::vector<float> plane_b;
    std::vector<uint8_t> band_emit_rgb;

    bool inference_done = false;
    while (!inference_done) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            unlock_source_once();
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }

        FailureInfo step_fail;
        if (!prepare_buffers_for_mode(step_fail)) {
            if (step_fail.status == EnhancerStatus::CANCELLED ||
                cancel_flag.load(std::memory_order_relaxed)) {
                unlock_source_once();
                result.status = EnhancerStatus::CANCELLED;
                return result;
            }
            FailureInfo terminal_fail;
            if (!advance_fallback_chain(step_fail, terminal_fail)) {
                unlock_source_once();
                result.status = terminal_fail.status;
                result.backend_used = backend_label;
                result.final_tile_size = active_tile;
                result.fallback_summary = build_fallback_summary();
                result.error_detail = terminal_fail.format();
                return result;
            }
            continue;
        }

        bool need_retry = false;
        const int passes_per_tile = active_tta ? 8 : 1;

        if (active_max_quality) {
            // =========================================================================
            // MAX QUALITY PATH:
            //   - 32px reflection prepadding
            //   - 32px 2D fixed-point (`uint16_t` Q8, 0..65280) feathered overlap blending
            //     (ZERO multi-tile float buffers anywhere — half the memory of float!)
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

            size_t row_channels = 0;
            checked_mul_size(static_cast<size_t>(plan.model_out_w), 3u, row_channels);

            std::vector<uint16_t> band_accum_q8;
            std::vector<uint16_t> prev_seam_q8;
            if (ny > 1) {
                size_t seam_elems = 0;
                checked_mul_size(static_cast<size_t>(kMaxQualityFeatherOut), row_channels, seam_elems);
                try {
                    prev_seam_q8.assign(seam_elems, 0u);
                } catch (const std::bad_alloc&) {
                    step_fail = FailureInfo{
                        EnhancerStatus::ERR_OUT_OF_MEMORY,
                        "std::bad_alloc(prev_seam_q8)",
                        static_cast<uint64_t>(seam_elems) * sizeof(uint16_t),
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                    need_retry = true;
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

                size_t band_elems = 0;
                checked_mul_size(static_cast<size_t>(band_h), row_channels, band_elems);
                try {
                    band_accum_q8.assign(band_elems, 0u);
                } catch (const std::bad_alloc&) {
                    step_fail = FailureInfo{
                        EnhancerStatus::ERR_OUT_OF_MEMORY,
                        "std::bad_alloc(band_accum_q8)",
                        static_cast<uint64_t>(band_elems) * sizeof(uint16_t),
                        active_tile,
                        backend_label,
                        0,
                        "band_h=" + std::to_string(band_h) + ", model_out_w=" + std::to_string(plan.model_out_w)
                    };
                    need_retry = true;
                    break;
                }

                for (int ix = 0; ix < nx; ++ix) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        unlock_source_once();
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
                    size_t tile_bytes = 0;
                    checked_image_bytes(tile_in_w, tile_in_h, 3, tile_bytes);

                    try {
                        if (tile_rgb.size() < tile_bytes) {
                            tile_rgb.resize(tile_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        step_fail = FailureInfo{
                            EnhancerStatus::ERR_OUT_OF_MEMORY,
                            "std::bad_alloc(tile_rgb)",
                            static_cast<uint64_t>(tile_bytes),
                            active_tile,
                            backend_label,
                            0,
                            ""
                        };
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
                        true,
                        tile_rgb.data()
                    );

                    const TilePassOutcome outcome = run_tile_inference_planar(
                        session,
                        m,
                        active_tile,
                        backend_label,
                        tile_rgb.data(),
                        tile_in_w,
                        tile_in_h,
                        active_tta,
                        tta_scratch_rgb,
                        plane_r,
                        plane_g,
                        plane_b,
                        step_fail,
                        cancel_flag,
                        [&](int) {
                            ++completed_subpasses;
                            const int pct = progress_start +
                                ((progress_end - progress_start) * completed_subpasses) / total_subpasses;
                            report_progress(pct, backend_label);
                        }
                    );

                    if (outcome == TilePassOutcome::CANCELLED) {
                        unlock_source_once();
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }
                    if (outcome != TilePassOutcome::OK) {
                        need_retry = true;
                        break;
                    }

                    const int tile_out_w = tile_in_w * m;
                    const int tile_out_h = tile_in_h * m;
                    const int roi_x0 = pad_left * m + (ext_x0 - in_x0 * m);
                    const int roi_y0 = pad_top * m + (band_y0 - in_y0 * m);

                    if (config.sharpness > 0) {
                        const uint64_t sharp_bytes =
                            static_cast<uint64_t>(ext_w) * static_cast<uint64_t>(band_h + 4) * sizeof(float) * 2u;
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
                            step_fail = FailureInfo{
                                EnhancerStatus::ERR_OUT_OF_MEMORY,
                                "std::bad_alloc(apply_adaptive_unsharp_mask_planar)",
                                sharp_bytes,
                                active_tile,
                                backend_label,
                                0,
                                ""
                            };
                            need_retry = true;
                            break;
                        }
                    }

                    std::vector<uint32_t> wx_q8_table(static_cast<size_t>(ext_w));
                    for (int lx = 0; lx < ext_w; ++lx) {
                        wx_q8_table[static_cast<size_t>(lx)] =
                            linear_tent_weight_q8(ext_x0 + lx, sx0, sx1, hl, hr);
                    }

                    #pragma omp parallel for schedule(static)
                    for (int by = 0; by < band_h; ++by) {
                        const size_t src_off =
                            static_cast<size_t>(roi_y0 + by) * static_cast<size_t>(tile_out_w) +
                            static_cast<size_t>(roi_x0);
                        const float* sr = plane_r.data() + src_off;
                        const float* sg = plane_g.data() + src_off;
                        const float* sb = plane_b.data() + src_off;

                        uint16_t* drow = band_accum_q8.data() +
                            static_cast<size_t>(by) * row_channels +
                            static_cast<size_t>(ext_x0) * 3u;

                        for (int lx = 0; lx < ext_w; ++lx) {
                            const uint32_t wx_q8 = wx_q8_table[static_cast<size_t>(lx)];
                            const size_t l3 = static_cast<size_t>(lx) * 3u;
                            drow[l3 + 0] = static_cast<uint16_t>(
                                std::min<uint32_t>(65280u, static_cast<uint32_t>(drow[l3 + 0]) + clamp_float_255_weighted_q8(sr[lx], wx_q8))
                            );
                            drow[l3 + 1] = static_cast<uint16_t>(
                                std::min<uint32_t>(65280u, static_cast<uint32_t>(drow[l3 + 1]) + clamp_float_255_weighted_q8(sg[lx], wx_q8))
                            );
                            drow[l3 + 2] = static_cast<uint16_t>(
                                std::min<uint32_t>(65280u, static_cast<uint32_t>(drow[l3 + 2]) + clamp_float_255_weighted_q8(sb[lx], wx_q8))
                            );
                        }
                    }
                }

                if (need_retry) {
                    break;
                }

                const int top_seam_rows = 2 * vt;
                const int bot_seam_rows = 2 * vb;
                const int emit_rows = band_h - bot_seam_rows;

                for (int by = 0; by < band_h; ++by) {
                    const uint32_t wy_q8 = linear_tent_weight_q8(band_y0 + by, sy0, sy1, vt, vb);
                    uint16_t* brow = band_accum_q8.data() + static_cast<size_t>(by) * row_channels;

                    if (by < top_seam_rows) {
                        const uint16_t* prev_row = prev_seam_q8.data() + static_cast<size_t>(by) * row_channels;
                        #pragma omp parallel for schedule(static)
                        for (size_t i = 0; i < row_channels; ++i) {
                            const uint32_t cur_scaled = (static_cast<uint32_t>(brow[i]) * wy_q8 + 128u) >> 8u;
                            brow[i] = static_cast<uint16_t>(
                                std::min<uint32_t>(65280u, static_cast<uint32_t>(prev_row[i]) + cur_scaled)
                            );
                        }
                    } else if (wy_q8 != 256u) {
                        #pragma omp parallel for schedule(static)
                        for (size_t i = 0; i < row_channels; ++i) {
                            brow[i] = static_cast<uint16_t>(
                                (static_cast<uint32_t>(brow[i]) * wy_q8 + 128u) >> 8u
                            );
                        }
                    }
                }

                if (bot_seam_rows > 0) {
                    const uint16_t* bot_src =
                        band_accum_q8.data() + static_cast<size_t>(emit_rows) * row_channels;
                    std::memcpy(
                        prev_seam_q8.data(),
                        bot_src,
                        static_cast<size_t>(bot_seam_rows) * row_channels * sizeof(uint16_t)
                    );
                }

                if (emit_rows > 0) {
                    uint8_t* dst_rows_u8 = nullptr;
                    size_t total_emit_vals = 0;
                    checked_mul_size(static_cast<size_t>(emit_rows), row_channels, total_emit_vals);

                    if (!plan.needs_post_resize()) {
                        dst_rows_u8 =
                            final_rgb.get() + static_cast<size_t>(emitted_model_rows) * row_channels;
                    } else {
                        try {
                            if (band_emit_rgb.size() < total_emit_vals) {
                                band_emit_rgb.resize(total_emit_vals);
                            }
                        } catch (const std::bad_alloc&) {
                            step_fail = FailureInfo{
                                EnhancerStatus::ERR_OUT_OF_MEMORY,
                                "std::bad_alloc(band_emit_rgb)",
                                static_cast<uint64_t>(total_emit_vals),
                                active_tile,
                                backend_label,
                                0,
                                ""
                            };
                            need_retry = true;
                            break;
                        }
                        dst_rows_u8 = band_emit_rgb.data();
                    }

                    const uint16_t* emit_src = band_accum_q8.data();
                    #pragma omp parallel for schedule(static)
                    for (size_t i = 0; i < total_emit_vals; ++i) {
                        dst_rows_u8[i] = static_cast<uint8_t>(
                            std::min<uint32_t>(255u, (static_cast<uint32_t>(emit_src[i]) + 128u) >> 8u)
                        );
                    }

                    if (plan.needs_post_resize()) {
                        if (!stream_resizer.push_source_rows(dst_rows_u8, emit_rows, cancel_flag)) {
                            if (cancel_flag.load(std::memory_order_relaxed)) {
                                unlock_source_once();
                                result.status = EnhancerStatus::CANCELLED;
                                return result;
                            }
                            step_fail = FailureInfo{
                                EnhancerStatus::ERR_INFERENCE,
                                "StreamingLanczos3Resizer::push_source_rows",
                                stream_resizer.ring_bytes(),
                                active_tile,
                                backend_label,
                                0,
                                ""
                            };
                            need_retry = true;
                            break;
                        }
                    }
                    emitted_model_rows += emit_rows;
                }
            }
        } else {
            // =========================================================================
            // STANDARD PATH:
            //   - 10px clamped prepadding + exact core crop directly into `final_rgb`
            //     (or single tile-row `band_emit_rgb` when Rule 3 Lanczos3 upscale applies)
            //   - Per-tile float buffers ONLY
            // =========================================================================
            const int tiles_x = (plan.work_w + active_tile - 1) / active_tile;
            const int tiles_y = (plan.work_h + active_tile - 1) / active_tile;
            const int total_subpasses = std::max(1, tiles_x * tiles_y * passes_per_tile);
            int completed_subpasses = 0;

            const size_t model_row_bytes = static_cast<size_t>(plan.model_out_w) * 3u;

            for (int ty = 0; ty < tiles_y && !need_retry; ++ty) {
                const int ry0 = ty * active_tile;
                const int rh = std::min(active_tile, plan.work_h - ry0);
                const int core_band_h = rh * m;

                uint8_t* band_base = nullptr;
                if (!plan.needs_post_resize()) {
                    band_base = final_rgb.get() +
                        static_cast<size_t>(ry0 * m) * model_row_bytes;
                } else {
                    size_t band_bytes = 0;
                    checked_mul_size(static_cast<size_t>(core_band_h), model_row_bytes, band_bytes);
                    try {
                        if (band_emit_rgb.size() < band_bytes) {
                            band_emit_rgb.resize(band_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        step_fail = FailureInfo{
                            EnhancerStatus::ERR_OUT_OF_MEMORY,
                            "std::bad_alloc(band_emit_rgb)",
                            static_cast<uint64_t>(band_bytes),
                            active_tile,
                            backend_label,
                            0,
                            ""
                        };
                        need_retry = true;
                        break;
                    }
                    band_base = band_emit_rgb.data();
                }

                for (int tx = 0; tx < tiles_x; ++tx) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        unlock_source_once();
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
                    size_t tile_bytes = 0;
                    checked_image_bytes(tile_in_w, tile_in_h, 3, tile_bytes);

                    try {
                        if (tile_rgb.size() < tile_bytes) {
                            tile_rgb.resize(tile_bytes);
                        }
                    } catch (const std::bad_alloc&) {
                        step_fail = FailureInfo{
                            EnhancerStatus::ERR_OUT_OF_MEMORY,
                            "std::bad_alloc(tile_rgb)",
                            static_cast<uint64_t>(tile_bytes),
                            active_tile,
                            backend_label,
                            0,
                            ""
                        };
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
                        m,
                        active_tile,
                        backend_label,
                        tile_rgb.data(),
                        tile_in_w,
                        tile_in_h,
                        active_tta,
                        tta_scratch_rgb,
                        plane_r,
                        plane_g,
                        plane_b,
                        step_fail,
                        cancel_flag,
                        [&](int) {
                            ++completed_subpasses;
                            const int pct = progress_start +
                                ((progress_end - progress_start) * completed_subpasses) / total_subpasses;
                            report_progress(pct, backend_label);
                        }
                    );

                    if (outcome == TilePassOutcome::CANCELLED) {
                        unlock_source_once();
                        result.status = EnhancerStatus::CANCELLED;
                        return result;
                    }
                    if (outcome != TilePassOutcome::OK) {
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
                        const uint64_t sharp_bytes =
                            static_cast<uint64_t>(core_out_w) * static_cast<uint64_t>(core_out_h + 4) * sizeof(float) * 2u;
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
                            step_fail = FailureInfo{
                                EnhancerStatus::ERR_OUT_OF_MEMORY,
                                "std::bad_alloc(apply_adaptive_unsharp_mask_planar)",
                                sharp_bytes,
                                active_tile,
                                backend_label,
                                0,
                                ""
                            };
                            need_retry = true;
                            break;
                        }
                    }

                    const size_t dst_x0 = static_cast<size_t>(rx0 * m);
                    for (int oy = 0; oy < core_out_h; ++oy) {
                        const size_t src_off =
                            static_cast<size_t>(crop_y0 + oy) * static_cast<size_t>(tile_out_w) +
                            static_cast<size_t>(crop_x0);
                        const float* sr = plane_r.data() + src_off;
                        const float* sg = plane_g.data() + src_off;
                        const float* sb = plane_b.data() + src_off;

                        uint8_t* dst_row = band_base +
                            static_cast<size_t>(oy) * model_row_bytes +
                            dst_x0 * 3u;
                        for (int ox = 0; ox < core_out_w; ++ox) {
                            const size_t ox3 = static_cast<size_t>(ox) * 3u;
                            dst_row[ox3 + 0] = clamp_float_255_to_u8(sr[ox]);
                            dst_row[ox3 + 1] = clamp_float_255_to_u8(sg[ox]);
                            dst_row[ox3 + 2] = clamp_float_255_to_u8(sb[ox]);
                        }
                    }
                }

                if (!need_retry && plan.needs_post_resize()) {
                    if (!stream_resizer.push_source_rows(band_base, core_band_h, cancel_flag)) {
                        if (cancel_flag.load(std::memory_order_relaxed)) {
                            unlock_source_once();
                            result.status = EnhancerStatus::CANCELLED;
                            return result;
                        }
                        step_fail = FailureInfo{
                            EnhancerStatus::ERR_INFERENCE,
                            "StreamingLanczos3Resizer::push_source_rows",
                            stream_resizer.ring_bytes(),
                            active_tile,
                            backend_label,
                            0,
                            ""
                        };
                        need_retry = true;
                        break;
                    }
                }
            }
        }

        if (need_retry) {
            FailureInfo terminal_fail;
            if (!advance_fallback_chain(step_fail, terminal_fail)) {
                unlock_source_once();
                result.status = terminal_fail.status;
                result.backend_used = backend_label;
                result.final_tile_size = active_tile;
                result.fallback_summary = build_fallback_summary();
                result.error_detail = terminal_fail.format();
                return result;
            }
            continue;
        }

        inference_done = true;
    }

    // Release source bitmap + Vulkan/ncnn session + per-tile scratch BEFORE encoding
    unlock_source_once();
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

    result.backend_used = backend_label;
    result.final_tile_size = active_tile;
    result.fallback_summary = build_fallback_summary();

    if (cancel_flag.load(std::memory_order_relaxed)) {
        result.status = EnhancerStatus::CANCELLED;
        return result;
    }

    // Optional min/max pixel range capture for synthetic self-test
    if (out_sample_stats_min_max != nullptr && final_rgb) {
        size_t total_bytes = 0;
        checked_image_bytes(plan.final_w, plan.final_h, 3, total_bytes);
        uint8_t vmin = 255u;
        uint8_t vmax = 0u;
        const size_t stride = std::max<size_t>(1u, total_bytes / 4096u);
        for (size_t i = 0; i < total_bytes; i += stride) {
            vmin = std::min(vmin, final_rgb[i]);
            vmax = std::max(vmax, final_rgb[i]);
        }
        out_sample_stats_min_max[0] = vmin;
        out_sample_stats_min_max[1] = vmax;
    }

    if (write_to_fd) {
        report_progress(95, backend_label);
        std::string encode_err;
        EnhancerStatus encode_status = EnhancerStatus::OK;
        uint64_t encode_req_bytes = 0;
        const bool write_ok = write_rgb_image_to_fd(
            owned_output_fd.get(),
            final_rgb.get(),
            plan.final_w,
            plan.final_h,
            config.format,
            config.jpeg_quality,
            &encode_err,
            &encode_status,
            &encode_req_bytes
        );
        const bool close_ok = owned_output_fd.close_now();
        final_rgb.reset();

        if (!write_ok || !close_ok) {
            const EnhancerStatus st = (!write_ok && encode_status != EnhancerStatus::OK)
                ? encode_status
                : EnhancerStatus::ERR_ENCODE_WRITE;
            result.status = st;
            result.error_detail = format_step_diagnostic(
                st,
                config.format == ImageFormat::PNG ? "write_png_streaming_zlib" : "write_jpg_full_chroma_444",
                encode_req_bytes,
                active_tile,
                backend_label,
                0,
                !encode_err.empty() ? encode_err : "close(output_fd) failed"
            );
            return result;
        }
    } else {
        final_rgb.reset();
    }

    report_progress(100, backend_label);
    result.status = EnhancerStatus::OK;

    size_t final_bytes = 0;
    checked_image_bytes(plan.final_w, plan.final_h, 3, final_bytes);
    result.error_detail = "[OK] out=" + std::to_string(plan.final_w) + "x" + std::to_string(plan.final_h) +
        " (" + std::to_string(final_bytes) + " B)" +
        " | tile=" + std::to_string(active_tile) +
        " | backend=" + backend_label +
        (gpu_heap_budget_mb > 0 ? (" | gpu_heap_budget=" + std::to_string(gpu_heap_budget_mb) + " MB") : "") +
        (result.fallback_summary.empty() ? "" : (" | fallbacks=" + result.fallback_summary));
    return result;
}

} // namespace

std::string query_vulkan_device_info() {
    int create_rc = 0;
    if (ensure_vulkan_ready(&create_rc)) {
        const int def_idx = ncnn::get_default_gpu_index();
        if (def_idx >= 0) {
            const ncnn::GpuInfo& gpu = ncnn::get_gpu_info(def_idx);
            const char* name = gpu.device_name();
            return std::string("Vulkan: ") + ((name && name[0] != '\0') ? name : "GPU");
        }
    }
    return "CPU only";
}

std::string query_vulkan_diagnostics_summary() {
    int create_rc = 0;
    const bool vk_ok = ensure_vulkan_ready(&create_rc);
    const int big_cores = ncnn::get_big_cpu_count();
    const int total_cores = ncnn::get_cpu_count();

    if (vk_ok) {
        const int def_idx = ncnn::get_default_gpu_index();
        ncnn::VulkanDevice* vkdev = (def_idx >= 0) ? ncnn::get_gpu_device(def_idx) : nullptr;
        const char* name = (def_idx >= 0) ? ncnn::get_gpu_info(def_idx).device_name() : nullptr;
        const uint32_t budget_mb = vkdev ? vkdev->get_heap_budget() : 0u;
        return std::string("Vulkan GPU: ") + ((name && name[0] != '\0') ? name : "GPU") +
            " | GPU Heap Budget: " + std::to_string(budget_mb) + " MB" +
            " | create_gpu_instance rc=" + std::to_string(create_rc) +
            " | CPU Cores: " + std::to_string(big_cores) + " big / " + std::to_string(total_cores) + " total";
    }

    return "Vulkan unavailable (create_gpu_instance rc=" + std::to_string(create_rc) +
        ", gpu_count=" + std::to_string(g_vulkan_instance_created ? ncnn::get_gpu_count() : 0) + ")" +
        " | CPU Cores: " + std::to_string(big_cores) + " big / " + std::to_string(total_cores) + " total";
}

void shutdown_vulkan_runtime() noexcept {
    std::lock_guard<std::mutex> lock(g_vulkan_mutex);
    if (g_vulkan_instance_created) {
        ncnn::destroy_gpu_instance();
        g_vulkan_instance_created = false;
        g_vulkan_init_attempted = false;
        g_vulkan_create_rc = 0;
    }
}

SelfTestReport run_native_self_test(AAssetManager* asset_manager) {
    SelfTestReport report;
    constexpr int kWidth = 256;
    constexpr int kHeight = 256;
    constexpr int kStride = kWidth * 4;
    constexpr size_t kSrcBytes = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4u;

    NativeByteBuffer synth_rgba = allocate_native_buffer(kSrcBytes);
    if (!synth_rgba) {
        const std::string oom_msg = format_step_diagnostic(
            EnhancerStatus::ERR_OUT_OF_MEMORY,
            "malloc(self_test_256x256_rgba)",
            kSrcBytes,
            64,
            "SelfTest",
            0,
            ""
        );
        report.vulkan = BackendSelfTestOutcome{false, 0, EnhancerStatus::ERR_OUT_OF_MEMORY, oom_msg};
        report.cpu = BackendSelfTestOutcome{false, 0, EnhancerStatus::ERR_OUT_OF_MEMORY, oom_msg};
        report.full_log = oom_msg;
        return report;
    }

    // Populate deterministic 256x256 RGB gradients + crisp diagonal/checker edges
    for (int y = 0; y < kHeight; ++y) {
        uint8_t* row = synth_rgba.get() + static_cast<size_t>(y) * kStride;
        for (int x = 0; x < kWidth; ++x) {
            const bool checker = (((x / 16) + (y / 16)) & 1) != 0;
            const bool diag = std::abs(x - y) <= 3;
            const uint8_t r = diag ? 250u : static_cast<uint8_t>((x * 220) / 255 + (checker ? 24 : 4));
            const uint8_t g = diag ? 60u : static_cast<uint8_t>((y * 220) / 255 + (checker ? 16 : 8));
            const uint8_t b = diag ? 30u : static_cast<uint8_t>(((255 - x) * 180) / 255 + (checker ? 40 : 12));
            const size_t x4 = static_cast<size_t>(x) * 4u;
            row[x4 + 0] = r;
            row[x4 + 1] = g;
            row[x4 + 2] = b;
            row[x4 + 3] = 255u;
        }
    }

    auto run_single_backend = [&](bool test_vulkan) -> BackendSelfTestOutcome {
        BackendSelfTestOutcome out;
        UpscaleConfig cfg;
        cfg.param_asset_path = "models/realesrgan-x4plus.param";
        cfg.bin_asset_path = "models/realesrgan-x4plus.bin";
        cfg.model_scale = 4;
        cfg.target_long_side = 0; // Native 4x -> 1024x1024
        cfg.requested_tile_size = 64;
        cfg.disable_fp16 = false;
        cfg.max_quality = false;
        cfg.tta_x8 = false;
        cfg.safe_mode = false;
        cfg.force_cpu = !test_vulkan;
        cfg.sharpness = 0;
        cfg.output_fd = -1;

        std::atomic<bool> never_cancel{false};
        uint8_t min_max[2] = {0u, 0u};

        const auto t0 = std::chrono::steady_clock::now();
        const UpscaleResult res = execute_pipeline_internal(
            asset_manager,
            synth_rgba.get(),
            kWidth,
            kHeight,
            kStride,
            nullptr,
            cfg,
            /*write_to_fd=*/false,
            /*allow_fallbacks=*/false,
            min_max,
            never_cancel,
            nullptr
        );
        const auto t1 = std::chrono::steady_clock::now();
        out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        out.status = res.status;

        if (res.status == EnhancerStatus::OK) {
            if (res.out_width == 1024 && res.out_height == 1024 && min_max[1] > min_max[0]) {
                out.passed = true;
                out.detail = "PASS (" + std::to_string(out.elapsed_ms) + " ms) | " +
                    res.backend_used + " | 256x256 -> " +
                    std::to_string(res.out_width) + "x" + std::to_string(res.out_height) +
                    " | tile=64 | pixel_range=[" + std::to_string(min_max[0]) + ".." +
                    std::to_string(min_max[1]) + "]";
            } else {
                out.passed = false;
                out.status = EnhancerStatus::ERR_INFERENCE;
                out.detail = format_step_diagnostic(
                    EnhancerStatus::ERR_INFERENCE,
                    "self_test_pixel_sanity",
                    1024ull * 1024ull * 3ull,
                    64,
                    res.backend_used,
                    0,
                    "degenerate output pixel_range=[" + std::to_string(min_max[0]) + ".." +
                        std::to_string(min_max[1]) + "]"
                );
            }
        } else {
            out.passed = false;
            out.detail = "FAIL (" + std::to_string(out.elapsed_ms) + " ms) | " + res.error_detail;
        }
        return out;
    };

    report.vulkan = run_single_backend(true);
    report.cpu = run_single_backend(false);

    report.full_log =
        "=== Enhancer Native Self-Test (256x256, realesrgan-x4plus, tile=64, maxQ=off, TTA=off) ===\n" +
        query_vulkan_diagnostics_summary() + "\n" +
        "[Vulkan] " + report.vulkan.detail + "\n" +
        "[CPU]    " + report.cpu.detail;
    LOGI("%s", report.full_log.c_str());
    return report;
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
    return execute_pipeline_internal(
        asset_manager,
        src_rgba,
        src_w,
        src_h,
        src_stride_bytes,
        unlock_bitmap_fn,
        config,
        /*write_to_fd=*/true,
        /*allow_fallbacks=*/true,
        nullptr,
        cancel_flag,
        progress_cb
    );
}

} // namespace enhancer
