#include "upscaler.h"
#include "resizer.h"

#include <allocator.h>
#include <cpu.h>
#include <gpu.h>
#include <mat.h>
#include <net.h>

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

#define LOG_TAG "EnhancerNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace enhancer {

namespace {

constexpr int kDefaultTileSize = 200;
constexpr int kMinTileSize = 48;
constexpr int kPrepad = 10;

std::mutex g_vk_mutex;
bool g_vk_init_attempted = false;
bool g_vk_instance_created = false;

bool ensure_vulkan_ready() {
    std::lock_guard<std::mutex> lock(g_vk_mutex);
    if (!g_vk_init_attempted) {
        g_vk_init_attempted = true;
        const int rc = ncnn::create_gpu_instance();
        g_vk_instance_created = (rc == 0);
        if (!g_vk_instance_created) {
            LOGW("ncnn::create_gpu_instance() returned %d; Vulkan unavailable", rc);
        }
    }
    if (!g_vk_instance_created) {
        return false;
    }
    return ncnn::get_gpu_count() > 0;
}

inline uint8_t float_norm_to_u8(float v) noexcept {
    const float scaled = v * 255.0f + 0.5f;
    if (!(scaled > 0.0f)) {
        return 0;
    }
    if (scaled >= 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(scaled);
}

struct TileRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Manages ncnn::Net and Vulkan allocators in strict destruction order
// (Net is always cleared BEFORE VkAllocators are reclaimed to VulkanDevice).
class NcnnSession {
public:
    NcnnSession() = default;
    ~NcnnSession() {
        reset();
    }

    NcnnSession(const NcnnSession&) = delete;
    NcnnSession& operator=(const NcnnSession&) = delete;

    bool init(
        AAssetManager* mgr,
        const std::string& param_path,
        const std::string& bin_path,
        bool request_vulkan,
        bool disable_fp16,
        std::string* out_error
    ) {
        reset();

        using_vulkan_ = false;
        backend_name_ = "CPU";

        const int big_cores = ncnn::get_big_cpu_count();
        const int all_cores = ncnn::get_cpu_count();
        const int threads = std::clamp(big_cores > 0 ? big_cores : all_cores, 1, 6);

        const bool fp16 = !disable_fp16;

        if (request_vulkan && ensure_vulkan_ready()) {
            const int gpu_idx = ncnn::get_default_gpu_index();
            vkdev_ = (gpu_idx >= 0) ? ncnn::get_gpu_device(gpu_idx) : nullptr;
            if (vkdev_ != nullptr) {
                blob_vkalloc_ = vkdev_->acquire_blob_allocator();
                staging_vkalloc_ = vkdev_->acquire_staging_allocator();

                net_.opt = ncnn::Option();
                net_.opt.lightmode = true;
                net_.opt.num_threads = threads;
                net_.opt.use_vulkan_compute = true;
                net_.opt.vulkan_device_index = gpu_idx;
                net_.opt.blob_vkallocator = blob_vkalloc_;
                net_.opt.workspace_vkallocator = blob_vkalloc_;
                net_.opt.staging_vkallocator = staging_vkalloc_;
                net_.opt.use_fp16_packed = fp16;
                net_.opt.use_fp16_storage = fp16;
                net_.opt.use_fp16_arithmetic = fp16;
                net_.opt.use_int8_packed = false;
                net_.opt.use_int8_storage = false;
                net_.opt.use_int8_arithmetic = false;

                net_.set_vulkan_device(vkdev_);

                const int rp = net_.load_param(mgr, param_path.c_str());
                const int rm = (rp == 0) ? net_.load_model(mgr, bin_path.c_str()) : rp;
                if (rp == 0 && rm == 0) {
                    using_vulkan_ = true;
                    const char* dev_name = ncnn::get_gpu_info(gpu_idx).device_name();
                    if (dev_name && dev_name[0] != '\0') {
                        backend_name_ = std::string("Vulkan (") + dev_name + ")";
                    } else {
                        backend_name_ = "Vulkan";
                    }
                    LOGI("Initialized ncnn on %s (fp16=%d)", backend_name_.c_str(), fp16 ? 1 : 0);
                    return true;
                }

                LOGW("Vulkan model load failed (param=%d, model=%d); resetting for CPU fallback", rp, rm);
                reset();
            }
        }

        if (request_vulkan) {
            // Caller requested Vulkan, so we only reach here if Vulkan couldn't initialize.
            if (out_error) {
                *out_error = "Vulkan initialization unavailable or failed";
            }
            return false;
        }

        // CPU initialization
        net_.opt = ncnn::Option();
        net_.opt.lightmode = true;
        net_.opt.num_threads = threads;
        net_.opt.use_vulkan_compute = false;
        net_.opt.use_fp16_packed = fp16;
        net_.opt.use_fp16_storage = fp16;
        net_.opt.use_fp16_arithmetic = fp16;
        net_.opt.use_int8_packed = false;
        net_.opt.use_int8_storage = false;
        net_.opt.use_int8_arithmetic = false;

        const int rp = net_.load_param(mgr, param_path.c_str());
        if (rp != 0) {
            if (out_error) {
                *out_error = "Failed to load model .param from assets: " + param_path;
            }
            reset();
            return false;
        }
        const int rm = net_.load_model(mgr, bin_path.c_str());
        if (rm != 0) {
            if (out_error) {
                *out_error = "Failed to load model .bin from assets: " + bin_path;
            }
            reset();
            return false;
        }

        using_vulkan_ = false;
        backend_name_ = "CPU";
        LOGI("Initialized ncnn on CPU (%d threads, fp16=%d)", threads, fp16 ? 1 : 0);
        return true;
    }

    void clear_allocator_pools() {
        if (blob_vkalloc_) {
            blob_vkalloc_->clear();
        }
        if (staging_vkalloc_) {
            staging_vkalloc_->clear();
        }
    }

    void reset() {
        // Clear Net first so no weight/blob buffers still reference VkAllocators
        net_.clear();
        if (vkdev_) {
            if (blob_vkalloc_) {
                blob_vkalloc_->clear();
                vkdev_->reclaim_blob_allocator(blob_vkalloc_);
                blob_vkalloc_ = nullptr;
            }
            if (staging_vkalloc_) {
                staging_vkalloc_->clear();
                vkdev_->reclaim_staging_allocator(staging_vkalloc_);
                staging_vkalloc_ = nullptr;
            }
            vkdev_ = nullptr;
        }
        using_vulkan_ = false;
    }

    ncnn::Net& net() { return net_; }
    bool using_vulkan() const { return using_vulkan_; }
    const std::string& backend_name() const { return backend_name_; }
    void set_backend_name(const std::string& name) { backend_name_ = name; }
    ncnn::VkAllocator* blob_vkalloc() const { return blob_vkalloc_; }
    ncnn::VkAllocator* staging_vkalloc() const { return staging_vkalloc_; }

private:
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::VkAllocator* blob_vkalloc_ = nullptr;
    ncnn::VkAllocator* staging_vkalloc_ = nullptr;
    ncnn::Net net_;
    bool using_vulkan_ = false;
    std::string backend_name_ = "CPU";
};

// Extracts a prepadded RGB888 tile with clamped border replication.
void extract_prepadded_tile_rgb(
    const uint8_t* work_rgb,
    const uint8_t* src_rgba,
    int src_rgba_stride,
    int img_w,
    int img_h,
    const TileRect& core,
    int pad_left,
    int pad_top,
    int tile_in_w,
    int tile_in_h,
    uint8_t* dst_tile_rgb
) {
    for (int ty = 0; ty < tile_in_h; ++ty) {
        const int sy = std::clamp(core.y - pad_top + ty, 0, img_h - 1);
        uint8_t* drow = dst_tile_rgb + static_cast<size_t>(ty) * static_cast<size_t>(tile_in_w) * 3;

        if (work_rgb != nullptr) {
            const uint8_t* srow = work_rgb + static_cast<size_t>(sy) * static_cast<size_t>(img_w) * 3;
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int sx = std::clamp(core.x - pad_left + tx, 0, img_w - 1);
                const uint8_t* spx = srow + static_cast<size_t>(sx) * 3;
                drow[tx * 3 + 0] = spx[0];
                drow[tx * 3 + 1] = spx[1];
                drow[tx * 3 + 2] = spx[2];
            }
        } else {
            const uint8_t* srow = src_rgba + static_cast<size_t>(sy) * static_cast<size_t>(src_rgba_stride);
            for (int tx = 0; tx < tile_in_w; ++tx) {
                const int sx = std::clamp(core.x - pad_left + tx, 0, img_w - 1);
                const uint8_t* spx = srow + static_cast<size_t>(sx) * 4;
                drow[tx * 3 + 0] = spx[0];
                drow[tx * 3 + 1] = spx[1];
                drow[tx * 3 + 2] = spx[2];
            }
        }
    }
}

void split_tile_front(
    std::deque<TileRect>& pending,
    const TileRect& rect,
    int max_tile
) {
    std::vector<TileRect> sub_tiles;
    for (int y = rect.y; y < rect.y + rect.h; y += max_tile) {
        const int sub_h = std::min(max_tile, rect.y + rect.h - y);
        for (int x = rect.x; x < rect.x + rect.w; x += max_tile) {
            const int sub_w = std::min(max_tile, rect.x + rect.w - x);
            sub_tiles.push_back(TileRect{x, y, sub_w, sub_h});
        }
    }
    for (auto it = sub_tiles.rbegin(); it != sub_tiles.rend(); ++it) {
        pending.push_front(*it);
    }
}

} // namespace

std::string query_vulkan_device_info() {
    if (!ensure_vulkan_ready()) {
        return "";
    }
    const int gpu_idx = ncnn::get_default_gpu_index();
    if (gpu_idx < 0) {
        return "";
    }
    const char* name = ncnn::get_gpu_info(gpu_idx).device_name();
    return (name != nullptr) ? std::string(name) : std::string("Vulkan GPU");
}

void shutdown_vulkan_instance() {
    std::lock_guard<std::mutex> lock(g_vk_mutex);
    if (g_vk_instance_created) {
        ncnn::destroy_gpu_instance();
        g_vk_instance_created = false;
        g_vk_init_attempted = false;
    }
}

UpscaleResult run_upscale_pipeline(
    AAssetManager* asset_mgr,
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    const std::function<void()>& unlock_bitmap_fn,
    const UpscaleConfig& cfg,
    UniqueFd fd_guard,
    const std::atomic<bool>& cancel_flag,
    const ProgressCallback& progress_cb
) {
    UpscaleResult res;
    bool bitmap_unlocked = false;
    auto unlock_source_once = [&]() noexcept {
        if (!bitmap_unlocked) {
            bitmap_unlocked = true;
            if (unlock_bitmap_fn) {
                unlock_bitmap_fn();
            }
        }
    };

    if (!asset_mgr || !src_rgba || src_w <= 0 || src_h <= 0 || src_stride_bytes < src_w * 4 ||
        (cfg.model_scale != 2 && cfg.model_scale != 3 && cfg.model_scale != 4) ||
        !fd_guard.valid()) {
        unlock_source_once();
        res.status = EnhancerStatus::ERR_INVALID_ARGS;
        res.error_detail = "Invalid input dimensions, scale, asset manager, or file descriptor";
        return res;
    }

    if (cancel_flag.load(std::memory_order_relaxed)) {
        unlock_source_once();
        res.status = EnhancerStatus::CANCELLED;
        return res;
    }

    const DimensionPlan plan = compute_dimensions(src_w, src_h, cfg.model_scale, cfg.target_long_side);
    res.out_width = plan.final_w;
    res.out_height = plan.final_h;

    const int initial_tile = (cfg.requested_tile_size > 0)
        ? std::max(kMinTileSize, cfg.requested_tile_size)
        : kDefaultTileSize;
    int active_tile = initial_tile;
    res.final_tile_size = active_tile;

    // Step 1: Optional area/box pre-downscale when source_long * m > target
    NativeByteBuffer work_rgb;
    if (plan.pre_downscaled) {
        const size_t work_bytes = static_cast<size_t>(plan.work_w) * static_cast<size_t>(plan.work_h) * 3;
        work_rgb = allocate_native_buffer(work_bytes);
        if (!work_rgb) {
            unlock_source_once();
            res.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            res.error_detail = "Out of memory allocating pre-downscale buffer";
            return res;
        }

        const bool downscale_ok = downscale_rgba_to_rgb_box(
            src_rgba,
            src_w,
            src_h,
            src_stride_bytes,
            work_rgb.get(),
            plan.work_w,
            plan.work_h,
            cancel_flag
        );
        // Unlock AndroidBitmap immediately now that downscaled RGB buffer is ready
        unlock_source_once();

        if (!downscale_ok) {
            res.status = cancel_flag.load(std::memory_order_relaxed)
                ? EnhancerStatus::CANCELLED
                : EnhancerStatus::ERR_INFERENCE;
            res.error_detail = "Pre-downscaling cancelled or failed";
            return res;
        }
    }

    // Step 2: Allocate Buffer 1 (model output RGB888)
    const size_t model_out_bytes =
        static_cast<size_t>(plan.model_out_w) * static_cast<size_t>(plan.model_out_h) * 3;
    NativeByteBuffer model_out_rgb = allocate_native_buffer(model_out_bytes);
    if (!model_out_rgb) {
        unlock_source_once();
        res.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        res.error_detail = "Out of memory allocating model output RGB buffer";
        return res;
    }

    // Step 3: Initialize ncnn (Vulkan preferred, automatic fallback to CPU if unavailable)
    NcnnSession session;
    bool used_cpu_fallback = false;
    std::string init_err;

    if (!session.init(asset_mgr, cfg.param_asset_path, cfg.bin_asset_path, true, cfg.disable_fp16, &init_err)) {
        // Automatic fallback to CPU
        used_cpu_fallback = true;
        init_err.clear();
        if (!session.init(asset_mgr, cfg.param_asset_path, cfg.bin_asset_path, false, cfg.disable_fp16, &init_err)) {
            unlock_source_once();
            res.status = EnhancerStatus::ERR_ASSET_LOAD;
            res.error_detail = init_err.empty() ? "Failed to load Real-ESRGAN model" : init_err;
            return res;
        }
        if (ensure_vulkan_ready()) {
            session.set_backend_name("CPU (Vulkan fallback)");
        } else {
            session.set_backend_name("CPU");
        }
    }

    auto report_progress = [&](int pct) {
        if (progress_cb) {
            progress_cb(std::clamp(pct, 0, 100), session.backend_name(), active_tile);
        }
    };

    report_progress(1);

    // Step 4: Mandatory tiled inference with 10px prepadding, border discard,
    // automatic tile halving (down to 48), and mid-run Vulkan -> CPU fallback.
    std::deque<TileRect> pending;
    for (int y = 0; y < plan.work_h; y += active_tile) {
        const int th = std::min(active_tile, plan.work_h - y);
        for (int x = 0; x < plan.work_w; x += active_tile) {
            const int tw = std::min(active_tile, plan.work_w - x);
            pending.push_back(TileRect{x, y, tw, th});
        }
    }

    const int m = cfg.model_scale;
    const uint64_t total_work_pixels =
        static_cast<uint64_t>(plan.work_w) * static_cast<uint64_t>(plan.work_h);
    uint64_t completed_work_pixels = 0;
    const int inference_end_pct = plan.post_upscaled ? 84 : 94;

    std::vector<uint8_t> tile_in_rgb;
    const float norm_vals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
    int last_tile_pct = 1;

    while (!pending.empty()) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            session.reset();
            unlock_source_once();
            res.status = EnhancerStatus::CANCELLED;
            res.backend_used = session.backend_name();
            res.used_cpu_fallback = used_cpu_fallback;
            res.final_tile_size = active_tile;
            return res;
        }

        const TileRect rect = pending.front();
        pending.pop_front();

        if (rect.w > active_tile || rect.h > active_tile) {
            split_tile_front(pending, rect, active_tile);
            continue;
        }

        const int pad_left = kPrepad;
        const int pad_top = kPrepad;
        const int pad_right = kPrepad + ((rect.w + 2 * kPrepad) & 1);
        const int pad_bottom = kPrepad + ((rect.h + 2 * kPrepad) & 1);
        const int tile_in_w = rect.w + pad_left + pad_right;
        const int tile_in_h = rect.h + pad_top + pad_bottom;

        bool tile_ok = false;
        bool bad_alloc_caught = false;

        try {
            const size_t tile_bytes = static_cast<size_t>(tile_in_w) * static_cast<size_t>(tile_in_h) * 3;
            if (tile_in_rgb.size() < tile_bytes) {
                tile_in_rgb.resize(tile_bytes);
            }

            extract_prepadded_tile_rgb(
                work_rgb.get(),
                src_rgba,
                src_stride_bytes,
                plan.work_w,
                plan.work_h,
                rect,
                pad_left,
                pad_top,
                tile_in_w,
                tile_in_h,
                tile_in_rgb.data()
            );

            ncnn::Mat in = ncnn::Mat::from_pixels(
                tile_in_rgb.data(),
                ncnn::Mat::PIXEL_RGB,
                tile_in_w,
                tile_in_h
            );

            if (!in.empty()) {
                in.substract_mean_normalize(nullptr, norm_vals);

                ncnn::Extractor ex = session.net().create_extractor();
                ex.set_light_mode(true);
                if (session.using_vulkan()) {
                    if (session.blob_vkalloc()) {
                        ex.set_blob_vkallocator(session.blob_vkalloc());
                        ex.set_workspace_vkallocator(session.blob_vkalloc());
                    }
                    if (session.staging_vkalloc()) {
                        ex.set_staging_vkallocator(session.staging_vkalloc());
                    }
                }

                const int r_in = ex.input("data", in);
                ncnn::Mat out;
                const int r_out = (r_in == 0) ? ex.extract("output", out) : r_in;

                const int expected_out_w = tile_in_w * m;
                const int expected_out_h = tile_in_h * m;

                if (r_in == 0 && r_out == 0 &&
                    !out.empty() &&
                    out.w == expected_out_w &&
                    out.h == expected_out_h &&
                    out.c >= 3) {

                    const ncnn::Mat ch_r = out.channel(0);
                    const ncnn::Mat ch_g = out.channel(1);
                    const ncnn::Mat ch_b = out.channel(2);

                    // Sanity-check against broken Vulkan driver NaN output
                    bool finite_ok = true;
                    if (session.using_vulkan()) {
                        const int mid_y = expected_out_h / 2;
                        const int mid_x = expected_out_w / 2;
                        const float sr = ch_r.row(mid_y)[mid_x];
                        const float sg = ch_g.row(mid_y)[mid_x];
                        const float sb = ch_b.row(mid_y)[mid_x];
                        finite_ok = std::isfinite(sr) && std::isfinite(sg) && std::isfinite(sb);
                    }

                    if (finite_ok) {
                        const int core_out_w = rect.w * m;
                        const int core_out_h = rect.h * m;
                        const int crop_x0 = pad_left * m;
                        const int crop_y0 = pad_top * m;

                        for (int cy = 0; cy < core_out_h; ++cy) {
                            const float* r_ptr = ch_r.row(crop_y0 + cy) + crop_x0;
                            const float* g_ptr = ch_g.row(crop_y0 + cy) + crop_x0;
                            const float* b_ptr = ch_b.row(crop_y0 + cy) + crop_x0;

                            uint8_t* dst_row = model_out_rgb.get() +
                                (static_cast<size_t>(rect.y * m + cy) * static_cast<size_t>(plan.model_out_w) +
                                 static_cast<size_t>(rect.x * m)) * 3;

                            for (int cx = 0; cx < core_out_w; ++cx) {
                                dst_row[cx * 3 + 0] = float_norm_to_u8(r_ptr[cx]);
                                dst_row[cx * 3 + 1] = float_norm_to_u8(g_ptr[cx]);
                                dst_row[cx * 3 + 2] = float_norm_to_u8(b_ptr[cx]);
                            }
                        }
                        tile_ok = true;
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            bad_alloc_caught = true;
            tile_ok = false;
        }

        if (!tile_ok) {
            session.clear_allocator_pools();

            // First recovery strategy: halve tile size down to minimum 48 and retry this tile
            if (active_tile > kMinTileSize && (rect.w > kMinTileSize || rect.h > kMinTileSize)) {
                const int prev_tile = active_tile;
                active_tile = std::max(kMinTileSize, active_tile / 2);
                LOGW("Tile (%d,%d %dx%d) failed on %s; halving tile size %d -> %d and retrying",
                     rect.x, rect.y, rect.w, rect.h,
                     session.backend_name().c_str(), prev_tile, active_tile);
                pending.push_front(rect);
                report_progress(last_tile_pct);
                continue;
            }

            // Second recovery strategy: if running on Vulkan, automatically retry on CPU
            if (session.using_vulkan()) {
                LOGW("Vulkan failed even at min tile size %d; switching to CPU fallback", active_tile);
                std::string cpu_err;
                session.reset();
                if (!session.init(asset_mgr, cfg.param_asset_path, cfg.bin_asset_path, false, cfg.disable_fp16, &cpu_err)) {
                    unlock_source_once();
                    res.status = EnhancerStatus::ERR_ASSET_LOAD;
                    res.error_detail = cpu_err.empty() ? "CPU fallback model load failed" : cpu_err;
                    return res;
                }
                used_cpu_fallback = true;
                session.set_backend_name("CPU (Vulkan fallback)");
                active_tile = std::max(kMinTileSize, std::min(initial_tile, kDefaultTileSize));
                pending.push_front(rect);
                report_progress(last_tile_pct);
                continue;
            }

            // Already on CPU at minimum tile size 48
            session.reset();
            unlock_source_once();
            res.status = bad_alloc_caught ? EnhancerStatus::ERR_OUT_OF_MEMORY : EnhancerStatus::ERR_INFERENCE;
            res.backend_used = session.backend_name();
            res.used_cpu_fallback = used_cpu_fallback;
            res.final_tile_size = active_tile;
            res.error_detail = "Tile inference failed on CPU even at minimum tile size 48";
            return res;
        }

        completed_work_pixels += static_cast<uint64_t>(rect.w) * static_cast<uint64_t>(rect.h);
        const int pct = 2 + static_cast<int>(
            (completed_work_pixels * static_cast<uint64_t>(inference_end_pct - 2)) /
            std::max<uint64_t>(1, total_work_pixels)
        );
        last_tile_pct = std::clamp(pct, 2, inference_end_pct);
        report_progress(last_tile_pct);
    }

    // Release source bitmap, pre-downscale buffer, tile scratch, and ncnn Net/Vulkan resources
    // BEFORE allocating any 8K Lanczos3 post-upscale buffer or running stb_image_write!
    const std::string final_backend = session.backend_name();
    tile_in_rgb.clear();
    tile_in_rgb.shrink_to_fit();
    work_rgb.reset();
    unlock_source_once();
    session.reset();

    res.backend_used = final_backend;
    res.used_cpu_fallback = used_cpu_fallback;
    res.final_tile_size = active_tile;

    // Step 5: Optional streaming row-by-row Lanczos3 upscale when source_long * m < target
    NativeByteBuffer final_rgb;
    const uint8_t* encode_rgb_ptr = model_out_rgb.get();

    if (plan.post_upscaled) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            res.status = EnhancerStatus::CANCELLED;
            return res;
        }

        const size_t final_bytes =
            static_cast<size_t>(plan.final_w) * static_cast<size_t>(plan.final_h) * 3;
        final_rgb = allocate_native_buffer(final_bytes);
        if (!final_rgb) {
            res.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            res.error_detail = "Out of memory allocating final target RGB buffer";
            return res;
        }

        const bool lanczos_ok = upscale_rgb_lanczos3_streaming(
            model_out_rgb.get(),
            plan.model_out_w,
            plan.model_out_h,
            final_rgb.get(),
            plan.final_w,
            plan.final_h,
            cancel_flag,
            [&](int resize_pct) {
                const int overall = 84 + (resize_pct * 11) / 100;
                if (progress_cb) {
                    progress_cb(std::clamp(overall, 84, 95), final_backend, active_tile);
                }
            }
        );

        // Free model_out_rgb immediately so only ONE big buffer exists during stb_image_write
        model_out_rgb.reset();

        if (!lanczos_ok) {
            res.status = cancel_flag.load(std::memory_order_relaxed)
                ? EnhancerStatus::CANCELLED
                : EnhancerStatus::ERR_OUT_OF_MEMORY;
            res.error_detail = "Lanczos3 post-upscaling cancelled or failed";
            return res;
        }

        encode_rgb_ptr = final_rgb.get();
    }

    // Step 6: Encode & stream to detached file descriptor via stb_image_write callback
    if (cancel_flag.load(std::memory_order_relaxed)) {
        res.status = EnhancerStatus::CANCELLED;
        return res;
    }

    if (progress_cb) {
        progress_cb(96, final_backend, active_tile);
    }

    std::string write_err;
    const bool write_ok = write_rgb_image_to_fd(
        fd_guard.get(),
        encode_rgb_ptr,
        plan.final_w,
        plan.final_h,
        cfg.format,
        cfg.jpeg_quality,
        &write_err
    );
    const bool close_ok = fd_guard.close_now();

    if (!write_ok || !close_ok) {
        res.status = EnhancerStatus::ERR_ENCODE_WRITE;
        res.error_detail = !write_err.empty() ? write_err : "Failed closing output file descriptor";
        return res;
    }

    if (progress_cb) {
        progress_cb(100, final_backend, active_tile);
    }

    res.status = EnhancerStatus::OK;
    return res;
}

} // namespace enhancer
