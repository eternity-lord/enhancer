#include "upscaler.h"

#include "face_restorer.h"
#include "image_io.h"
#include "photo_enhance_ops.h"
#include "resizer.h"
#include "tile_ops.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
constexpr int kStandardTilePad = 16;          // >= 10px prepadding (64px at 4x) with reflect border
constexpr int kMaxQualityTilePad = 32;        // 32px prepadding (128px at 4x) with reflect border
constexpr int kStandardOverlapHalfIn = 3;     // 6*m output feather (24px at 4x), exact multiple of m
constexpr int kMaxQualityOverlapHalfIn = 4;   // 8*m output feather (32px at 4x), exact multiple of m
constexpr uint32_t kQualcommVendorId = 0x5143u;

// Balanced process-lifetime Vulkan instance manager:
// `create_gpu_instance()` is called lazily at most once, and `destroy_gpu_instance()` is called
// once on `shutdown_vulkan_runtime()` (`JNI_OnUnload`) iff creation succeeded (`rc == 0`).
std::mutex g_vulkan_mutex;
bool g_vulkan_init_attempted = false;
bool g_vulkan_instance_created = false;
int g_vulkan_create_rc = 0;

std::mutex g_fp16_calib_mutex;
std::mutex g_calib_run_mutex;
bool g_fp16_calib_done = false;
Fp16CalibrationReport g_fp16_calib_report;

bool contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (!needle || needle[0] == '\0') {
        return false;
    }
    const size_t nlen = std::strlen(needle);
    if (haystack.size() < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

std::string format_fixed(double val, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, val);
    return std::string(buf);
}

std::string format_hex32(uint32_t val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%04x", val);
    return std::string(buf);
}

bool is_adreno_gpu(uint32_t vendor_id, const std::string& device_name) {
    return vendor_id == kQualcommVendorId || contains_case_insensitive(device_name, "adreno");
}

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

        blob_cpu_allocator_.set_size_compare_ratio(0.0f);
        workspace_cpu_allocator_.set_size_compare_ratio(0.0f);

        net_.opt = ncnn::Option();
        net_.opt.num_threads = threads;
        net_.opt.lightmode = true;
        net_.opt.blob_allocator = &blob_cpu_allocator_;
        net_.opt.workspace_allocator = &workspace_cpu_allocator_;
        net_.opt.use_int8_packed = false;
        net_.opt.use_int8_storage = false;
        net_.opt.use_int8_arithmetic = false;
        net_.opt.use_bf16_packed = false;
        net_.opt.use_bf16_storage = false;
        net_.opt.use_cooperative_matrix = false;
        cpu_threads_ = threads;

        if (config.disable_fp16) {
            net_.opt.use_fp16_packed = false;
            net_.opt.use_fp16_storage = false;
            net_.opt.use_fp16_arithmetic = false;
            net_.opt.use_fp16_uniform = false;
        } else {
            net_.opt.use_fp16_packed = true;
            net_.opt.use_fp16_storage = true;
            // Keep fp16_arithmetic = false to prevent deep RRDB FP16 overflow / NaN on mobile GPUs
            net_.opt.use_fp16_arithmetic = false;
            net_.opt.use_fp16_uniform = true;
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

            // Disable Winograd F(4,3) on Vulkan: across 345+ 3x3 RRDB layers its 8x per-axis
            // (64x 2D) Vandermonde transform amplifies 4x4 block rounding into a 16x16 smooth-area
            // mesh, and winograd_gemm.comp preloads shared memory before checking gx < outw.
            // Winograd F(2,3) + SGEMM remain active for fast Vulkan compute without 4x4 mesh error.
            net_.opt.use_winograd43_convolution = false;
            net_.opt.use_vulkan_compute = true;
            net_.opt.vulkan_device_index = gpu_idx;
            net_.set_vulkan_device(vkdev_);
        } else {
            net_.opt.use_vulkan_compute = false;
            // Direct conv on CPU avoids multi-GB Winograd/SGEMM workspace spikes on 256+ px tiles.
            net_.opt.use_winograd_convolution = false;
            net_.opt.use_sgemm_convolution = false;
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
        blob_cpu_allocator_.clear();
        workspace_cpu_allocator_.clear();
    }

    void release() noexcept {
        // Clear Net first so no buffers reference VkAllocators, CPU allocators, or VulkanDevice
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
        blob_cpu_allocator_.clear();
        workspace_cpu_allocator_.clear();
        use_vulkan_ = false;
    }

    ncnn::Net& net() noexcept { return net_; }
    bool using_vulkan() const noexcept { return use_vulkan_; }
    int cpu_threads() const noexcept { return cpu_threads_; }
    ncnn::VkAllocator* blob_vkallocator() const noexcept { return blob_vkallocator_; }
    ncnn::VkAllocator* staging_vkallocator() const noexcept { return staging_vkallocator_; }
    ncnn::PoolAllocator* blob_cpu_allocator() noexcept { return &blob_cpu_allocator_; }
    ncnn::PoolAllocator* workspace_cpu_allocator() noexcept { return &workspace_cpu_allocator_; }
    const char* input_blob() const noexcept { return input_blob_.c_str(); }
    const char* output_blob() const noexcept { return output_blob_.c_str(); }

private:
    ncnn::Net net_;
    ncnn::PoolAllocator blob_cpu_allocator_;
    ncnn::PoolAllocator workspace_cpu_allocator_;
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::VkAllocator* blob_vkallocator_ = nullptr;
    ncnn::VkAllocator* staging_vkallocator_ = nullptr;
    bool use_vulkan_ = false;
    int cpu_threads_ = 4;
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
                ex.set_blob_allocator(session.blob_cpu_allocator());
                ex.set_workspace_allocator(session.workspace_cpu_allocator());
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

                        // Runtime Vulkan tile integrity verifier: catches NaN/Inf, shader overflow,
                        // zero/gray constant buffer collapse, brightness corruption, and chroma collapse.
                        if (session.using_vulkan()) {
                            const int gx_cnt = std::min(16, pass_in_w);
                            const int gy_cnt = std::min(16, pass_in_h);
                            double in_sum_y = 0.0, in_sum_y2 = 0.0, in_chroma = 0.0;
                            double out_sum_y = 0.0, out_sum_y2 = 0.0, out_chroma = 0.0;
                            size_t n_samp = 0;

                            for (int gy = 0; gy < gy_cnt && !pass_failed; ++gy) {
                                const int iy = (gy * pass_in_h + pass_in_h / 2) / gy_cnt;
                                const int oy = std::min(expected_h - 1, iy * model_scale + model_scale / 2);
                                for (int gx = 0; gx < gx_cnt; ++gx) {
                                    const int ix = (gx * pass_in_w + pass_in_w / 2) / gx_cnt;
                                    const int ox = std::min(expected_w - 1, ix * model_scale + model_scale / 2);
                                    const size_t oidx = static_cast<size_t>(oy) * static_cast<size_t>(expected_w) +
                                        static_cast<size_t>(ox);

                                    const float rf = ch_r[oidx];
                                    const float gf = ch_g[oidx];
                                    const float bf = ch_b[oidx];
                                    if (!std::isfinite(rf) || !std::isfinite(gf) || !std::isfinite(bf) ||
                                        rf < -2.0f || rf > 3.0f ||
                                        gf < -2.0f || gf > 3.0f ||
                                        bf < -2.0f || bf > 3.0f) {
                                        out_failure = FailureInfo{
                                            EnhancerStatus::ERR_VULKAN_CORRUPTED,
                                            "vulkan_nan_or_overflow_tile",
                                            static_cast<uint64_t>(expected_out_bytes),
                                            nominal_tile_size,
                                            backend_label,
                                            0,
                                            "non-finite or out-of-range float in Vulkan output tile"
                                        };
                                        pass_failed = true;
                                        break;
                                    }

                                    const uint8_t* ipx = pass_rgb +
                                        (static_cast<size_t>(iy) * static_cast<size_t>(pass_in_w) +
                                         static_cast<size_t>(ix)) * 3u;
                                    const double ir = static_cast<double>(ipx[0]);
                                    const double ig = static_cast<double>(ipx[1]);
                                    const double ib = static_cast<double>(ipx[2]);
                                    const double iy_val = 0.299 * ir + 0.587 * ig + 0.114 * ib;
                                    in_sum_y += iy_val;
                                    in_sum_y2 += iy_val * iy_val;
                                    in_chroma += std::fabs(ir - ig) + std::fabs(ig - ib);

                                    const double or_v = std::clamp(static_cast<double>(rf) * 255.0, 0.0, 255.0);
                                    const double og_v = std::clamp(static_cast<double>(gf) * 255.0, 0.0, 255.0);
                                    const double ob_v = std::clamp(static_cast<double>(bf) * 255.0, 0.0, 255.0);
                                    const double oy_val = 0.299 * or_v + 0.587 * og_v + 0.114 * ob_v;
                                    out_sum_y += oy_val;
                                    out_sum_y2 += oy_val * oy_val;
                                    out_chroma += std::fabs(or_v - og_v) + std::fabs(og_v - ob_v);
                                    ++n_samp;
                                }
                            }

                            if (!pass_failed && n_samp >= 16) {
                                const double inv_n = 1.0 / static_cast<double>(n_samp);
                                const double in_mean = in_sum_y * inv_n;
                                const double out_mean = out_sum_y * inv_n;
                                const double in_std = std::sqrt(std::max(0.0, in_sum_y2 * inv_n - in_mean * in_mean));
                                const double out_std = std::sqrt(std::max(0.0, out_sum_y2 * inv_n - out_mean * out_mean));
                                const double in_chr = in_chroma * inv_n;
                                const double out_chr = out_chroma * inv_n;

                                const bool gray_or_zero_collapse = (in_std > 6.0 && out_std < 0.35);
                                const bool brightness_corrupted = (std::fabs(out_mean - in_mean) > 75.0);
                                const bool chroma_collapsed = (in_chr > 18.0 && out_chr < 1.0);

                                if (gray_or_zero_collapse || brightness_corrupted || chroma_collapsed) {
                                    out_failure = FailureInfo{
                                        EnhancerStatus::ERR_VULKAN_CORRUPTED,
                                        "vulkan_corrupted_or_gray_tile",
                                        static_cast<uint64_t>(expected_out_bytes),
                                        nominal_tile_size,
                                        backend_label,
                                        0,
                                        "in_mean=" + format_fixed(in_mean, 1) +
                                            ", out_mean=" + format_fixed(out_mean, 1) +
                                            ", in_std=" + format_fixed(in_std, 1) +
                                            ", out_std=" + format_fixed(out_std, 2) +
                                            ", in_chr=" + format_fixed(in_chr, 1) +
                                            ", out_chr=" + format_fixed(out_chr, 2)
                                    };
                                    pass_failed = true;
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

        // `Extractor ex` and `Mat out` are now destroyed; flush per-tile Vulkan & CPU pools immediately!
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

inline int align_up_int(int val, int align) noexcept {
    return ((val + align - 1) / align) * align;
}

// Unified feathered tiled inference engine shared by BOTH Standard and Max Quality modes
// AND by the 700x500 Diagnostics Seam Test:
//   - Balanced grid splits (`build_grid_splits`) so right-edge and bottom-edge tiles are never narrow slivers.
//   - Uniform aligned padded tile shape (`fixed_tile_in_w x fixed_tile_in_h`) across EVERY tile in the image
//     so interior, right-column, and bottom-row tiles execute identical Vulkan/CPU tensor geometries.
//   - True symmetric border reflection (`reflect_coord`) on all 4 borders (`>= 16px` Standard, `32px` MaxQ).
//   - Exact `float` tent feather weights (`w_prev + w_curr == 1.0f`) accumulated in compact per-seam `float`
//     strip buffers (`prev_seam_flt`, `cur_bot_seam_flt`, `right_col_seam_flt`) — NEVER a full-image float buffer!
TilePassOutcome run_feathered_tiled_inference(
    ScopedModelSession& session,
    int m,
    int active_tile,
    bool max_quality,
    bool active_tta,
    int sharpness,
    const std::string& backend_label,
    const uint8_t* work_ptr,
    bool work_is_rgba,
    int work_w,
    int work_h,
    size_t work_stride,
    int model_out_w,
    bool needs_post_resize,
    uint8_t* final_rgb_ptr,
    StreamingLanczos3Resizer* stream_resizer,
    std::vector<uint8_t>& tile_rgb,
    std::vector<uint8_t>& tta_scratch_rgb,
    std::vector<float>& plane_r,
    std::vector<float>& plane_g,
    std::vector<float>& plane_b,
    std::vector<uint8_t>& band_emit_rgb,
    std::vector<int>* out_x_splits,
    std::vector<int>* out_y_splits,
    int* out_fixed_tile_w,
    int* out_fixed_tile_h,
    FailureInfo& out_failure,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int completed_subpasses, int total_subpasses)>& on_progress
) {
    const int prepad_in = max_quality ? kMaxQualityTilePad : kStandardTilePad;
    const int overlap_half_in = max_quality ? kMaxQualityOverlapHalfIn : kStandardOverlapHalfIn;
    const int half_feather_out = overlap_half_in * m;
    const int feather_out = 2 * half_feather_out;
    const int min_in_cell = std::max(8, 2 * overlap_half_in + 2);

    const std::vector<int> x_splits = build_grid_splits(work_w, active_tile, min_in_cell);
    const std::vector<int> y_splits = build_grid_splits(work_h, active_tile, min_in_cell);
    if (out_x_splits) *out_x_splits = x_splits;
    if (out_y_splits) *out_y_splits = y_splits;

    const int nx = static_cast<int>(x_splits.size()) - 1;
    const int ny = static_cast<int>(y_splits.size()) - 1;
    const int passes_per_tile = active_tta ? 8 : 1;
    const int total_subpasses = std::max(1, nx * ny * passes_per_tile);
    int completed_subpasses = 0;

    // Compute uniform padded tile input shape across ALL tiles in this upscale pass.
    int max_core_in_w = 0;
    for (int ix = 0; ix < nx; ++ix) {
        const int in_x0 = x_splits[static_cast<size_t>(ix)] - ((ix > 0) ? overlap_half_in : 0);
        const int in_x1 = x_splits[static_cast<size_t>(ix + 1)] + ((ix + 1 < nx) ? overlap_half_in : 0);
        max_core_in_w = std::max(max_core_in_w, in_x1 - in_x0);
    }
    int max_core_in_h = 0;
    for (int iy = 0; iy < ny; ++iy) {
        const int in_y0 = y_splits[static_cast<size_t>(iy)] - ((iy > 0) ? overlap_half_in : 0);
        const int in_y1 = y_splits[static_cast<size_t>(iy + 1)] + ((iy + 1 < ny) ? overlap_half_in : 0);
        max_core_in_h = std::max(max_core_in_h, in_y1 - in_y0);
    }

    const int align_h = (active_tile >= 96) ? 32 : 16;
    const int fixed_tile_in_w = align_up_int(max_core_in_w + 2 * prepad_in, 16);
    const int fixed_tile_in_h = align_up_int(max_core_in_h + 2 * prepad_in, align_h);
    if (out_fixed_tile_w) *out_fixed_tile_w = fixed_tile_in_w;
    if (out_fixed_tile_h) *out_fixed_tile_h = fixed_tile_in_h;

    size_t tile_bytes = 0;
    if (!checked_image_bytes(fixed_tile_in_w, fixed_tile_in_h, 3, tile_bytes)) {
        out_failure = FailureInfo{
            EnhancerStatus::ERR_INVALID_ARGS,
            "checked_image_bytes(fixed_tile_rgb)",
            0,
            active_tile,
            backend_label,
            0,
            ""
        };
        return TilePassOutcome::FAILED;
    }

    size_t row_channels = 0;
    if (!checked_mul_size(static_cast<size_t>(model_out_w), 3u, row_channels)) {
        out_failure = FailureInfo{
            EnhancerStatus::ERR_INVALID_ARGS,
            "checked_mul_size(row_channels)",
            0,
            active_tile,
            backend_label,
            0,
            ""
        };
        return TilePassOutcome::FAILED;
    }

    // Small per-seam float overlap strips ONLY (never a full-image float buffer!)
    std::vector<float> prev_seam_flt;
    std::vector<float> cur_bot_seam_flt;
    std::vector<float> right_col_seam_flt;

    try {
        tile_rgb.assign(tile_bytes, 0u);
        if (ny > 1) {
            size_t v_seam_elems = 0;
            if (!checked_mul_size(static_cast<size_t>(feather_out), row_channels, v_seam_elems)) {
                throw std::bad_alloc();
            }
            prev_seam_flt.assign(v_seam_elems, 0.0f);
            cur_bot_seam_flt.assign(v_seam_elems, 0.0f);
        }
    } catch (const std::bad_alloc&) {
        const uint64_t approx_bytes = static_cast<uint64_t>(tile_bytes) +
            2ull * static_cast<uint64_t>(feather_out) * static_cast<uint64_t>(row_channels) * sizeof(float);
        out_failure = FailureInfo{
            EnhancerStatus::ERR_OUT_OF_MEMORY,
            "std::bad_alloc(seam_float_strips)",
            approx_bytes,
            active_tile,
            backend_label,
            0,
            ""
        };
        return TilePassOutcome::FAILED;
    }

    int emitted_model_rows = 0;

    for (int iy = 0; iy < ny; ++iy) {
        const int in_y0 = y_splits[static_cast<size_t>(iy)] - ((iy > 0) ? overlap_half_in : 0);
        const int in_y1 = y_splits[static_cast<size_t>(iy + 1)] + ((iy + 1 < ny) ? overlap_half_in : 0);
        const int sy0 = y_splits[static_cast<size_t>(iy)] * m;
        const int sy1 = y_splits[static_cast<size_t>(iy + 1)] * m;
        const int vt = (iy > 0) ? half_feather_out : 0;
        const int vb = (iy + 1 < ny) ? half_feather_out : 0;

        const int band_y0 = in_y0 * m; // == sy0 - vt identically!
        const int band_y1 = in_y1 * m; // == sy1 + vb identically!
        const int band_h = band_y1 - band_y0;
        const int top_seam_rows = 2 * vt;
        const int bot_seam_rows = 2 * vb;
        const int emit_rows = band_h - bot_seam_rows;

        if (bot_seam_rows > 0) {
            std::fill(cur_bot_seam_flt.begin(), cur_bot_seam_flt.end(), 0.0f);
        }

        if (nx > 1) {
            size_t h_seam_elems = 0;
            checked_mul3_size(static_cast<size_t>(band_h), static_cast<size_t>(feather_out), 3u, h_seam_elems);
            try {
                right_col_seam_flt.assign(h_seam_elems, 0.0f);
            } catch (const std::bad_alloc&) {
                out_failure = FailureInfo{
                    EnhancerStatus::ERR_OUT_OF_MEMORY,
                    "std::bad_alloc(right_col_seam_flt)",
                    static_cast<uint64_t>(h_seam_elems) * sizeof(float),
                    active_tile,
                    backend_label,
                    0,
                    ""
                };
                return TilePassOutcome::FAILED;
            }
        }

        uint8_t* emit_dst_base = nullptr;
        if (emit_rows > 0) {
            if (!needs_post_resize) {
                emit_dst_base = final_rgb_ptr + static_cast<size_t>(emitted_model_rows) * row_channels;
            } else {
                size_t emit_bytes = 0;
                checked_mul_size(static_cast<size_t>(emit_rows), row_channels, emit_bytes);
                try {
                    if (band_emit_rgb.size() < emit_bytes) {
                        band_emit_rgb.resize(emit_bytes);
                    }
                } catch (const std::bad_alloc&) {
                    out_failure = FailureInfo{
                        EnhancerStatus::ERR_OUT_OF_MEMORY,
                        "std::bad_alloc(band_emit_rgb)",
                        static_cast<uint64_t>(emit_bytes),
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                    return TilePassOutcome::FAILED;
                }
                emit_dst_base = band_emit_rgb.data();
            }
        }

        for (int ix = 0; ix < nx; ++ix) {
            if (cancel_flag.load(std::memory_order_relaxed)) {
                return TilePassOutcome::CANCELLED;
            }

            const int in_x0 = x_splits[static_cast<size_t>(ix)] - ((ix > 0) ? overlap_half_in : 0);
            const int in_x1 = x_splits[static_cast<size_t>(ix + 1)] + ((ix + 1 < nx) ? overlap_half_in : 0);
            const int sx0 = x_splits[static_cast<size_t>(ix)] * m;
            const int sx1 = x_splits[static_cast<size_t>(ix + 1)] * m;
            const int hl = (ix > 0) ? half_feather_out : 0;
            const int hr = (ix + 1 < nx) ? half_feather_out : 0;

            const int ext_x0 = in_x0 * m; // == sx0 - hl identically!
            const int ext_x1 = in_x1 * m; // == sx1 + hr identically!
            const int ext_w = ext_x1 - ext_x0;

            const int pad_left = prepad_in;
            const int pad_top = prepad_in;

            // Zero scratch and populate full uniform tile (`fixed_tile_in_w x fixed_tile_in_h`)
            // with true symmetric border reflection (`reflect_pad = true`).
            std::fill(tile_rgb.begin(), tile_rgb.end(), static_cast<uint8_t>(0));
            fill_padded_tile_rgb(
                work_ptr,
                work_is_rgba,
                work_w,
                work_h,
                work_stride,
                in_x0,
                in_y0,
                pad_left,
                pad_top,
                fixed_tile_in_w,
                fixed_tile_in_h,
                /*reflect_pad=*/true,
                tile_rgb.data()
            );

            const TilePassOutcome outcome = run_tile_inference_planar(
                session,
                m,
                active_tile,
                backend_label,
                tile_rgb.data(),
                fixed_tile_in_w,
                fixed_tile_in_h,
                active_tta,
                tta_scratch_rgb,
                plane_r,
                plane_g,
                plane_b,
                out_failure,
                cancel_flag,
                [&](int) {
                    ++completed_subpasses;
                    if (on_progress) {
                        on_progress(completed_subpasses, total_subpasses);
                    }
                }
            );

            if (outcome != TilePassOutcome::OK) {
                return outcome;
            }

            const int tile_out_w = fixed_tile_in_w * m;
            const int tile_out_h = fixed_tile_in_h * m;
            const int roi_x0 = pad_left * m;
            const int roi_y0 = pad_top * m;

            if (sharpness > 0) {
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
                        sharpness
                    );
                } catch (const std::bad_alloc&) {
                    out_failure = FailureInfo{
                        EnhancerStatus::ERR_OUT_OF_MEMORY,
                        "std::bad_alloc(apply_adaptive_unsharp_mask_planar)",
                        sharp_bytes,
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                    return TilePassOutcome::FAILED;
                }
            }

            const int left_ov_w = 2 * hl;
            const int right_ov_w = 2 * hr;
            const int ready_w = ext_w - right_ov_w;

            std::vector<float> wx_table(static_cast<size_t>(ext_w));
            for (int lx = 0; lx < ext_w; ++lx) {
                wx_table[static_cast<size_t>(lx)] = linear_tent_weight(ext_x0 + lx, sx0, sx1, hl, hr);
            }

            #pragma omp parallel for schedule(static)
            for (int by = 0; by < band_h; ++by) {
                const float wy = linear_tent_weight(band_y0 + by, sy0, sy1, vt, vb);
                const size_t src_off =
                    static_cast<size_t>(roi_y0 + by) * static_cast<size_t>(tile_out_w) +
                    static_cast<size_t>(roi_x0);
                const float* sr = plane_r.data() + src_off;
                const float* sg = plane_g.data() + src_off;
                const float* sb = plane_b.data() + src_off;

                // 1) Blend horizontally complete columns [0 .. ready_w) and apply vertical weight `wy`
                for (int lx = 0; lx < ready_w; ++lx) {
                    const int gx = ext_x0 + lx;
                    const float wx = wx_table[static_cast<size_t>(lx)];
                    float hr_val = std::clamp(sr[lx], 0.0f, 255.0f) * wx;
                    float hg_val = std::clamp(sg[lx], 0.0f, 255.0f) * wx;
                    float hb_val = std::clamp(sb[lx], 0.0f, 255.0f) * wx;

                    if (lx < left_ov_w) {
                        const float* lseam = right_col_seam_flt.data() +
                            (static_cast<size_t>(by) * static_cast<size_t>(feather_out) + static_cast<size_t>(lx)) * 3u;
                        hr_val += lseam[0];
                        hg_val += lseam[1];
                        hb_val += lseam[2];
                    }

                    if (by < emit_rows) {
                        float fr_val = hr_val * wy;
                        float fg_val = hg_val * wy;
                        float fb_val = hb_val * wy;
                        if (by < top_seam_rows) {
                            const float* tseam = prev_seam_flt.data() +
                                static_cast<size_t>(by) * row_channels + static_cast<size_t>(gx) * 3u;
                            fr_val += tseam[0];
                            fg_val += tseam[1];
                            fb_val += tseam[2];
                        }
                        uint8_t* dp = emit_dst_base +
                            static_cast<size_t>(by) * row_channels + static_cast<size_t>(gx) * 3u;
                        dp[0] = clamp_float_255_to_u8(fr_val);
                        dp[1] = clamp_float_255_to_u8(fg_val);
                        dp[2] = clamp_float_255_to_u8(fb_val);
                    } else {
                        float* bseam = cur_bot_seam_flt.data() +
                            static_cast<size_t>(by - emit_rows) * row_channels + static_cast<size_t>(gx) * 3u;
                        bseam[0] = hr_val * wy;
                        bseam[1] = hg_val * wy;
                        bseam[2] = hb_val * wy;
                    }
                }

                // 2) Store right horizontal overlap [ready_w .. ext_w) into `right_col_seam_flt` for tile `ix + 1`
                for (int rx = 0; rx < right_ov_w; ++rx) {
                    const int lx = ready_w + rx;
                    const float wx = wx_table[static_cast<size_t>(lx)];
                    float* rseam = right_col_seam_flt.data() +
                        (static_cast<size_t>(by) * static_cast<size_t>(feather_out) + static_cast<size_t>(rx)) * 3u;
                    rseam[0] = std::clamp(sr[lx], 0.0f, 255.0f) * wx;
                    rseam[1] = std::clamp(sg[lx], 0.0f, 255.0f) * wx;
                    rseam[2] = std::clamp(sb[lx], 0.0f, 255.0f) * wx;
                }
            }
        }

        if (bot_seam_rows > 0) {
            prev_seam_flt.swap(cur_bot_seam_flt);
        }

        if (emit_rows > 0) {
            if (needs_post_resize && stream_resizer != nullptr) {
                if (!stream_resizer->push_source_rows(emit_dst_base, emit_rows, cancel_flag)) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        return TilePassOutcome::CANCELLED;
                    }
                    out_failure = FailureInfo{
                        EnhancerStatus::ERR_INFERENCE,
                        "StreamingLanczos3Resizer::push_source_rows",
                        stream_resizer->ring_bytes(),
                        active_tile,
                        backend_label,
                        0,
                        ""
                    };
                    return TilePassOutcome::FAILED;
                }
            }
            emitted_model_rows += emit_rows;
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
    {
        // Wait if one-shot startup FP16 calibration is currently running so two Vulkan sessions never overlap.
        std::lock_guard<std::mutex> wait_calib(g_calib_run_mutex);
    }

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
    bool active_disable_fp16 = config.disable_fp16;
    bool tried_alt_vulkan_fp = false;
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

    // -------------------------------------------------------------------------
    // Stage 0 & Stage 1A: Compact work_rgb_buf preparation, Old Photo restore,
    // Pre-SR Denoise/Deblock, and SCRFD 5-point Face Detection BEFORE SR load!
    // Unlocks Java Bitmap immediately so 2 full buffers NEVER coexist.
    // -------------------------------------------------------------------------
    NativeByteBuffer work_rgb_buf;
    const uint8_t* work_ptr = src_rgba;
    bool work_is_rgba = true;
    size_t work_stride = static_cast<size_t>(src_stride_bytes);
    int cur_work_w = src_w;
    int cur_work_h = src_h;
    std::vector<NormalizedFace> detected_faces;

    const DimensionPlan initial_plan = compute_dimensions(
        src_w,
        src_h,
        config.model_scale,
        config.target_long_side,
        active_max_quality
    );
    if (initial_plan.final_w <= 0 || initial_plan.final_h <= 0 ||
        initial_plan.work_w <= 0 || initial_plan.work_h <= 0) {
        unlock_source_once();
        result.status = EnhancerStatus::ERR_INVALID_ARGS;
        result.error_detail = format_step_diagnostic(
            EnhancerStatus::ERR_INVALID_ARGS,
            "compute_dimensions(initial)",
            0,
            active_tile,
            active_vulkan ? "Vulkan" : "CPU",
            0,
            ""
        );
        return result;
    }

    const bool want_old_photo = (config.mode == EnhanceMode::OLD_PHOTO);
    float effective_denoise = std::clamp(config.denoise_strength, 0.0f, 1.0f);
    if (config.mode == EnhanceMode::STRONG && effective_denoise < 0.45f) {
        effective_denoise = 0.45f;
    } else if (config.mode == EnhanceMode::BALANCED && effective_denoise < 0.14f) {
        effective_denoise = 0.14f;
    }
    const bool want_pre_denoise = (effective_denoise > 0.01f);
    const bool want_faces = (config.enhance_faces && config.face_strength > 0.02f);
    const bool need_initial_work_rgb =
        initial_plan.pre_downscaled || want_old_photo || want_pre_denoise || want_faces;

    if (need_initial_work_rgb) {
        size_t init_work_bytes = 0;
        if (!checked_image_bytes(initial_plan.work_w, initial_plan.work_h, 3, init_work_bytes)) {
            unlock_source_once();
            result.status = EnhancerStatus::ERR_INVALID_ARGS;
            result.error_detail = "checked_image_bytes(initial_work_rgb) overflow";
            return result;
        }
        report_progress(2, active_vulkan ? "Preparing" : "CPU");
        work_rgb_buf = allocate_native_buffer(init_work_bytes);
        if (!work_rgb_buf) {
            unlock_source_once();
            result.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
            result.error_detail = format_step_diagnostic(
                EnhancerStatus::ERR_OUT_OF_MEMORY,
                "malloc(initial_work_rgb)",
                static_cast<uint64_t>(init_work_bytes),
                active_tile,
                active_vulkan ? "Vulkan" : "CPU",
                0,
                ""
            );
            return result;
        }

        if (initial_plan.pre_downscaled) {
            if (!downscale_rgba_to_rgb_box(
                    src_rgba,
                    src_w,
                    src_h,
                    src_stride_bytes,
                    work_rgb_buf.get(),
                    initial_plan.work_w,
                    initial_plan.work_h,
                    cancel_flag
                )) {
                unlock_source_once();
                result.status = cancel_flag.load() ? EnhancerStatus::CANCELLED : EnhancerStatus::ERR_INFERENCE;
                return result;
            }
        } else {
            #pragma omp parallel for schedule(static) if (src_h >= 64)
            for (int y = 0; y < src_h; ++y) {
                const uint8_t* srow = src_rgba + static_cast<size_t>(y) * static_cast<size_t>(src_stride_bytes);
                uint8_t* drow = work_rgb_buf.get() + static_cast<size_t>(y) * static_cast<size_t>(src_w) * 3u;
                for (int x = 0; x < src_w; ++x) {
                    drow[x * 3 + 0] = srow[x * 4 + 0];
                    drow[x * 3 + 1] = srow[x * 4 + 1];
                    drow[x * 3 + 2] = srow[x * 4 + 2];
                }
            }
        }

        // Unlock & release Java ARGB_8888 Bitmap immediately!
        unlock_source_once();
        cur_work_w = initial_plan.work_w;
        cur_work_h = initial_plan.work_h;
        work_ptr = work_rgb_buf.get();
        work_is_rgba = false;
        work_stride = static_cast<size_t>(cur_work_w) * 3u;

        if (want_old_photo) {
            if (!apply_old_photo_pre_restore(work_rgb_buf.get(), cur_work_w, cur_work_h, 0.85f, cancel_flag)) {
                result.status = EnhancerStatus::CANCELLED;
                return result;
            }
        }
        if (want_pre_denoise) {
            if (!apply_pre_denoise_deblock(work_rgb_buf.get(), cur_work_w, cur_work_h, effective_denoise, cancel_flag)) {
                result.status = EnhancerStatus::CANCELLED;
                return result;
            }
        }
        if (want_faces) {
            detected_faces = detect_faces_scrfd(asset_manager, work_rgb_buf.get(), cur_work_w, cur_work_h);
            result.faces_detected = static_cast<int>(detected_faces.size());
        }
    }

    if (cancel_flag.load(std::memory_order_relaxed)) {
        unlock_source_once();
        result.status = EnhancerStatus::CANCELLED;
        return result;
    }

    // Initialize model session ONCE per job (re-initialized ONLY if fallback switches precision or CPU)
    ScopedModelSession session;
    std::string backend_label;
    uint32_t gpu_heap_budget_mb = 0;
    FailureInfo init_failure;

    auto make_session_cfg = [&]() -> UpscaleConfig {
        UpscaleConfig scfg = config;
        scfg.disable_fp16 = active_disable_fp16;
        return scfg;
    };

    if (active_vulkan) {
        if (session.init(
                asset_manager,
                make_session_cfg(),
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
                make_session_cfg(),
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
    report_progress(4, backend_label);

    // Buffers managed across retries:
    // INVARIANT: At most ONE full-size output buffer (`final_rgb`) ever exists at any instant!
    DimensionPlan plan;
    bool plan_prepared = false;
    bool prepared_for_max_quality = false;

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

        // Pre-downscale into compact `work_rgb_buf` when Standard Rule 2 applies
        if (plan.pre_downscaled && (cur_work_w != plan.work_w || cur_work_h != plan.work_h)) {
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

            NativeByteBuffer next_work_buf = allocate_native_buffer(work_bytes);
            if (!next_work_buf) {
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

            if (work_rgb_buf) {
                // Downscale existing RGB work_rgb_buf (safe even after src_rgba was unlocked!)
                const double sx_scale = static_cast<double>(cur_work_w) / static_cast<double>(plan.work_w);
                const double sy_scale = static_cast<double>(cur_work_h) / static_cast<double>(plan.work_h);
                const uint8_t* sbase = work_rgb_buf.get();
                uint8_t* dbase = next_work_buf.get();
                #pragma omp parallel for schedule(static) if (plan.work_h >= 32)
                for (int dy = 0; dy < plan.work_h; ++dy) {
                    const int sy0 = std::clamp(static_cast<int>(dy * sy_scale), 0, cur_work_h - 1);
                    const int sy1 = std::clamp(static_cast<int>((dy + 1) * sy_scale + 0.999), sy0 + 1, cur_work_h);
                    for (int dx = 0; dx < plan.work_w; ++dx) {
                        const int sx0 = std::clamp(static_cast<int>(dx * sx_scale), 0, cur_work_w - 1);
                        const int sx1 = std::clamp(static_cast<int>((dx + 1) * sx_scale + 0.999), sx0 + 1, cur_work_w);
                        uint32_t acc_r = 0, acc_g = 0, acc_b = 0, cnt = 0;
                        for (int sy = sy0; sy < sy1; ++sy) {
                            const uint8_t* srow = sbase + (static_cast<size_t>(sy) * cur_work_w + sx0) * 3u;
                            for (int sx = sx0; sx < sx1; ++sx) {
                                acc_r += srow[(sx - sx0) * 3 + 0];
                                acc_g += srow[(sx - sx0) * 3 + 1];
                                acc_b += srow[(sx - sx0) * 3 + 2];
                                ++cnt;
                            }
                        }
                        uint8_t* dpx = dbase + (static_cast<size_t>(dy) * plan.work_w + dx) * 3u;
                        dpx[0] = static_cast<uint8_t>((acc_r + cnt / 2) / std::max(1u, cnt));
                        dpx[1] = static_cast<uint8_t>((acc_g + cnt / 2) / std::max(1u, cnt));
                        dpx[2] = static_cast<uint8_t>((acc_b + cnt / 2) / std::max(1u, cnt));
                    }
                }
                work_rgb_buf = std::move(next_work_buf);
            } else {
                const bool ok_down = downscale_rgba_to_rgb_box(
                    src_rgba,
                    src_w,
                    src_h,
                    src_stride_bytes,
                    next_work_buf.get(),
                    plan.work_w,
                    plan.work_h,
                    cancel_flag
                );
                if (!ok_down) {
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
                work_rgb_buf = std::move(next_work_buf);
                unlock_source_once();
            }

            cur_work_w = plan.work_w;
            cur_work_h = plan.work_h;
            work_ptr = work_rgb_buf.get();
            work_is_rgba = false;
            work_stride = static_cast<size_t>(plan.work_w) * 3u;
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

    // Ordered fallback state machine:
    //   - On Vulkan shader/precision corruption (`ERR_VULKAN_CORRUPTED`): immediately toggle
    //     Vulkan FP16 <-> Vulkan FP32 once, and if still corrupted switch straight to CPU without
    //     unnecessarily sacrificing tile size, TTA, or Max quality!
    //   - On OOM / resource failures: halve tile (down to 48) -> disable TTA -> disable Max quality -> CPU.
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

        if (cause.status == EnhancerStatus::ERR_VULKAN_CORRUPTED && active_vulkan) {
            if (!tried_alt_vulkan_fp) {
                tried_alt_vulkan_fp = true;
                active_disable_fp16 = !active_disable_fp16;
                session.release();
                uint32_t unused_budget = 0;
                FailureInfo alt_err;
                if (session.init(
                        asset_manager,
                        make_session_cfg(),
                        true,
                        active_tile,
                        backend_label,
                        unused_budget,
                        alt_err
                    )) {
                    record_fallback(
                        std::string("toggled Vulkan ") + (active_disable_fp16 ? "FP16->FP32" : "FP32->FP16")
                    );
                    result.backend_used = backend_label;
                    return true;
                }
            }

            // Switch straight to CPU backend while preserving user's quality settings
            active_vulkan = false;
            result.used_cpu_fallback = true;
            session.release();
            active_tile = std::max(kMinTileSize, std::min(requested_base_tile, 128));
            result.final_tile_size = active_tile;
            record_fallback("switched to CPU backend (Vulkan output corrupted)");

            uint32_t unused_budget = 0;
            FailureInfo cpu_err;
            if (!session.init(
                    asset_manager,
                    make_session_cfg(),
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
                    make_session_cfg(),
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
    const int progress_end = !detected_faces.empty() ? 78 : 88;

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
        const TilePassOutcome tiled_outcome = run_feathered_tiled_inference(
            session,
            m,
            active_tile,
            active_max_quality,
            active_tta,
            config.sharpness,
            backend_label,
            work_ptr,
            work_is_rgba,
            plan.work_w,
            plan.work_h,
            work_stride,
            plan.model_out_w,
            plan.needs_post_resize(),
            final_rgb.get(),
            plan.needs_post_resize() ? &stream_resizer : nullptr,
            tile_rgb,
            tta_scratch_rgb,
            plane_r,
            plane_g,
            plane_b,
            band_emit_rgb,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            step_fail,
            cancel_flag,
            [&](int completed_subpasses, int total_subpasses) {
                const int pct = progress_start +
                    ((progress_end - progress_start) * completed_subpasses) / total_subpasses;
                report_progress(pct, backend_label);
            }
        );

        if (tiled_outcome == TilePassOutcome::CANCELLED) {
            unlock_source_once();
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }
        if (tiled_outcome != TilePassOutcome::OK) {
            need_retry = true;
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

    // Release source bitmap + Vulkan/ncnn session + per-tile scratch BEFORE Stage 3/4/5!
    const int saved_cpu_threads = session.cpu_threads();
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

    // -------------------------------------------------------------------------
    // Stage 3: Let's Enhance "Photo 2.0 Faces" (CodeFormer 512x512 Restoration)
    // Runs ONLY after SR session & work_rgb_buf are freed so peak RAM stays minimal!
    // -------------------------------------------------------------------------
    if (want_faces && !detected_faces.empty() && final_rgb) {
        report_progress(80, backend_label);
        result.faces_restored = restore_faces_codeformer(
            asset_manager,
            final_rgb.get(),
            plan.final_w,
            plan.final_h,
            detected_faces,
            config.face_strength,
            active_vulkan,
            active_disable_fp16,
            saved_cpu_threads,
            cancel_flag,
            [&](int done_faces, int total_faces) {
                const int pct = 80 + (10 * done_faces) / std::max(1, total_faces);
                report_progress(pct, backend_label);
            }
        );
        if (cancel_flag.load(std::memory_order_relaxed)) {
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }
    }

    // -------------------------------------------------------------------------
    // Stage 4A: Let's Enhance "Ultra" Multi-Scale Detail & Clarity Synthesis
    // -------------------------------------------------------------------------
    if (config.mode == EnhanceMode::ULTRA && config.ultra_intensity > 0.01f && final_rgb) {
        report_progress(91, backend_label);
        if (!apply_ultra_multiscale_detail(
                final_rgb.get(),
                plan.final_w,
                plan.final_h,
                config.ultra_intensity,
                config.ultra_size,
                cancel_flag
            )) {
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }
    }

    // -------------------------------------------------------------------------
    // Stage 4B: Let's Enhance "Light AI" Smart HDR Tone, Exposure & Vibrance
    // -------------------------------------------------------------------------
    if (config.light_ai && config.light_ai_strength > 0.01f && final_rgb) {
        report_progress(93, backend_label);
        if (!apply_light_ai_hdr(
                final_rgb.get(),
                plan.final_w,
                plan.final_h,
                config.light_ai_strength,
                cancel_flag
            )) {
            result.status = EnhancerStatus::CANCELLED;
            return result;
        }
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
        report_progress(96, backend_label);
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
            &encode_req_bytes,
            config.dpi
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
    result.error_detail = "[OK] mode=" + std::string(enhance_mode_name(config.mode)) +
        " | out=" + std::to_string(plan.final_w) + "x" + std::to_string(plan.final_h) +
        " (" + std::to_string(final_bytes) + " B, " + std::to_string(config.dpi) + " DPI)" +
        " | tile=" + std::to_string(active_tile) +
        " | backend=" + backend_label +
        (result.faces_detected > 0
            ? (" | faces=" + std::to_string(result.faces_restored) + "/" + std::to_string(result.faces_detected))
            : "") +
        (config.light_ai ? " | LightAI=ON" : "") +
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

bool should_default_disable_fp16_sync() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        if (g_fp16_calib_done) {
            return g_fp16_calib_report.recommend_disable_fp16;
        }
    }
    // Default to standard FP16 storage + FP32 accumulation (`disable_fp16 = false`).
    // Unconditionally forcing pure FP32 storage (`rgba32f`) on Adreno GPUs can trigger
    // driver storage-image bugs; empirical calibration below only enables FP32 storage
    // when verified cleaner AND valid against CPU reference.
    return false;
}

Fp16CalibrationReport calibrate_vulkan_fp16_policy(AAssetManager* asset_manager) {
    std::lock_guard<std::mutex> run_lock(g_calib_run_mutex);
    {
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        if (g_fp16_calib_done) {
            return g_fp16_calib_report;
        }
    }

    Fp16CalibrationReport report;
    int create_rc = 0;
    const bool vk_ok = ensure_vulkan_ready(&create_rc);
    if (!vk_ok) {
        report.vulkan_available = false;
        report.is_adreno = false;
        report.recommend_disable_fp16 = false;
        report.reason = "FP16 default: N/A — CPU FP32 backend (no Vulkan GPU detected)";
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        g_fp16_calib_report = report;
        g_fp16_calib_done = true;
        return report;
    }

    const int def_idx = ncnn::get_default_gpu_index();
    if (def_idx < 0) {
        report.vulkan_available = false;
        report.recommend_disable_fp16 = false;
        report.reason = "FP16 default: N/A — default Vulkan GPU index < 0";
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        g_fp16_calib_report = report;
        g_fp16_calib_done = true;
        return report;
    }

    const ncnn::GpuInfo& info = ncnn::get_gpu_info(def_idx);
    const char* raw_name = info.device_name();
    report.vulkan_available = true;
    report.vendor_id = info.vendor_id();
    report.gpu_name = (raw_name && raw_name[0] != '\0') ? raw_name : "GPU";
    report.is_adreno = is_adreno_gpu(report.vendor_id, report.gpu_name);

    // Run lightweight 2-tile calibration (96x96 vs 64x96 over identical smooth interior patch)
    // comparing Vulkan fp16 ON and Vulkan fp16 OFF against CPU FP32 reference.
    if (asset_manager != nullptr) {
        constexpr int kBigW = 96;
        constexpr int kBigH = 96;
        constexpr int kSmallW = 64;
        constexpr int kSmallH = 96;

        std::vector<uint8_t> big_rgb(static_cast<size_t>(kBigW * kBigH * 3), 0u);
        std::vector<uint8_t> small_rgb(static_cast<size_t>(kSmallW * kSmallH * 3), 0u);

        for (int y = 0; y < kBigH; ++y) {
            for (int x = 0; x < kBigW; ++x) {
                const bool bright_floor = (y >= kBigH / 2);
                const uint8_t base = bright_floor
                    ? static_cast<uint8_t>(216 + (x * 20) / kBigW)
                    : static_cast<uint8_t>(128 + (y * 16) / kBigH);
                const size_t idx = static_cast<size_t>(y * kBigW + x) * 3u;
                big_rgb[idx + 0] = base;
                big_rgb[idx + 1] = base;
                big_rgb[idx + 2] = static_cast<uint8_t>( std::min(255, static_cast<int>(base) + 2) );
                if (x < kSmallW) {
                    const size_t sidx = static_cast<size_t>(y * kSmallW + x) * 3u;
                    small_rgb[sidx + 0] = big_rgb[idx + 0];
                    small_rgb[sidx + 1] = big_rgb[idx + 1];
                    small_rgb[sidx + 2] = big_rgb[idx + 2];
                }
            }
        }

        auto run_patch = [&](bool use_vk, bool disable_fp16, const uint8_t* rgb, int w, int h,
                             std::vector<float>& pr, std::vector<float>& pg, std::vector<float>& pb) -> bool {
            UpscaleConfig cfg;
            cfg.param_asset_path = "models/realesrgan-x4plus.param";
            cfg.bin_asset_path = "models/realesrgan-x4plus.bin";
            cfg.model_scale = 4;
            cfg.disable_fp16 = disable_fp16;
            cfg.force_cpu = !use_vk;

            ScopedModelSession sess;
            std::string backend_lbl;
            uint32_t budget_mb = 0;
            FailureInfo fail;
            if (!sess.init(asset_manager, cfg, use_vk, 64, backend_lbl, budget_mb, fail)) {
                return false;
            }
            std::vector<uint8_t> tta_tmp;
            std::atomic<bool> never_cancel{false};
            const TilePassOutcome rc = run_tile_inference_planar(
                sess, 4, 64, backend_lbl, rgb, w, h, false,
                tta_tmp, pr, pg, pb, fail, never_cancel, nullptr
            );
            return rc == TilePassOutcome::OK;
        };

        std::vector<float> cpu_r, cpu_g, cpu_b;
        std::vector<float> vk16_big_r, vk16_big_g, vk16_big_b;
        std::vector<float> vk16_sm_r, vk16_sm_g, vk16_sm_b;
        std::vector<float> vk32_big_r, vk32_big_g, vk32_big_b;

        const bool ok_cpu = run_patch(false, true, big_rgb.data(), kBigW, kBigH, cpu_r, cpu_g, cpu_b);
        const bool ok_vk16_big = ok_cpu &&
            run_patch(true, false, big_rgb.data(), kBigW, kBigH, vk16_big_r, vk16_big_g, vk16_big_b);
        const bool ok_vk16_sm = ok_vk16_big &&
            run_patch(true, false, small_rgb.data(), kSmallW, kSmallH, vk16_sm_r, vk16_sm_g, vk16_sm_b);
        const bool ok_vk32_big = ok_cpu &&
            run_patch(true, true, big_rgb.data(), kBigW, kBigH, vk32_big_r, vk32_big_g, vk32_big_b);

        if (ok_cpu && ok_vk16_big && ok_vk16_sm && ok_vk32_big) {
            // Compare shared interior ROI [in_x = 16..40, in_y = 16..80] -> [out_x = 64..160, out_y = 64..320]
            constexpr int kOx0 = 64;
            constexpr int kOx1 = 160;
            constexpr int kOy0 = 64;
            constexpr int kOy1 = 320;
            constexpr int kBigOutW = kBigW * 4;
            constexpr int kSmallOutW = kSmallW * 4;

            double sum_16_cpu = 0.0;
            double max_16_cpu = 0.0;
            double sum_32_cpu = 0.0;
            double sum_16_big = 0.0;
            double sum_16_sm = 0.0;
            size_t cnt = 0;

            for (int oy = kOy0; oy < kOy1; ++oy) {
                for (int ox = kOx0; ox < kOx1; ++ox) {
                    const size_t bi = static_cast<size_t>(oy * kBigOutW + ox);
                    const size_t si = static_cast<size_t>(oy * kSmallOutW + ox);

                    const double c_lum = (cpu_r[bi] + cpu_g[bi] + cpu_b[bi]) / 3.0;
                    const double v16_lum = (vk16_big_r[bi] + vk16_big_g[bi] + vk16_big_b[bi]) / 3.0;
                    const double v16s_lum = (vk16_sm_r[si] + vk16_sm_g[si] + vk16_sm_b[si]) / 3.0;
                    const double v32_lum = (vk32_big_r[bi] + vk32_big_g[bi] + vk32_big_b[bi]) / 3.0;

                    const double d16 = std::abs(v16_lum - c_lum);
                    const double d32 = std::abs(v32_lum - c_lum);
                    sum_16_cpu += d16;
                    max_16_cpu = std::max(max_16_cpu, d16);
                    sum_32_cpu += d32;
                    sum_16_big += v16_lum;
                    sum_16_sm += v16s_lum;
                    ++cnt;
                }
            }

            if (cnt > 0) {
                report.fp16_vs_cpu_mean_diff = static_cast<float>(sum_16_cpu / static_cast<double>(cnt));
                report.fp16_vs_cpu_max_diff = static_cast<float>(max_16_cpu);
                report.fp32_vs_cpu_mean_diff = static_cast<float>(sum_32_cpu / static_cast<double>(cnt));
                report.fp16_inter_tile_diff = static_cast<float>(
                    std::abs(sum_16_big - sum_16_sm) / static_cast<double>(cnt)
                );
            }

            const bool fp16_drifts =
                report.fp16_vs_cpu_mean_diff > 0.50f ||
                report.fp16_vs_cpu_max_diff > 2.0f ||
                report.fp16_inter_tile_diff > 0.25f;
            const bool fp32_clean_and_better =
                report.fp32_vs_cpu_mean_diff < 1.50f &&
                (report.fp32_vs_cpu_mean_diff + 0.12f < report.fp16_vs_cpu_mean_diff ||
                 report.fp16_inter_tile_diff > 0.25f);

            report.recommend_disable_fp16 = fp16_drifts && fp32_clean_and_better;
            if (report.recommend_disable_fp16) {
                report.reason = "FP16 default: OFF (verified cleaner FP32 storage on " +
                    report.gpu_name + " [" + format_hex32(report.vendor_id) + "]; " +
                    "vk_fp16 vs CPU mean=" + format_fixed(report.fp16_vs_cpu_mean_diff, 2) +
                    " max=" + format_fixed(report.fp16_vs_cpu_max_diff, 2) +
                    " tileShift=" + format_fixed(report.fp16_inter_tile_diff, 2) +
                    ", vk_fp32 vs CPU mean=" + format_fixed(report.fp32_vs_cpu_mean_diff, 2) + " lvl)";
            } else {
                report.reason = "FP16 default: ON (" + report.gpu_name +
                    " [" + format_hex32(report.vendor_id) + "] calibrated vs CPU: " +
                    "vk_fp16 mean=" + format_fixed(report.fp16_vs_cpu_mean_diff, 2) +
                    " max=" + format_fixed(report.fp16_vs_cpu_max_diff, 2) +
                    " tileShift=" + format_fixed(report.fp16_inter_tile_diff, 2) +
                    ", vk_fp32 mean=" + format_fixed(report.fp32_vs_cpu_mean_diff, 2) + " lvl)";
            }

            std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
            g_fp16_calib_report = report;
            g_fp16_calib_done = true;
            return report;
        } else if (ok_cpu && !ok_vk16_big && ok_vk32_big) {
            report.recommend_disable_fp16 = true;
            report.reason = "FP16 default: OFF (Vulkan FP16 failed verification, FP32 storage passed on " +
                report.gpu_name + " [" + format_hex32(report.vendor_id) + "])";
            std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
            g_fp16_calib_report = report;
            g_fp16_calib_done = true;
            return report;
        }
    }

    report.recommend_disable_fp16 = false;
    report.reason = "FP16 storage + FP32 accum default: ON — " + report.gpu_name +
        " [" + format_hex32(report.vendor_id) + "]";

    {
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        g_fp16_calib_report = report;
        g_fp16_calib_done = true;
    }
    return report;
}

std::string query_vulkan_diagnostics_summary() {
    int create_rc = 0;
    const bool vk_ok = ensure_vulkan_ready(&create_rc);
    const int big_cores = ncnn::get_big_cpu_count();
    const int total_cores = ncnn::get_cpu_count();

    std::string fp16_line;
    {
        std::lock_guard<std::mutex> lock(g_fp16_calib_mutex);
        if (g_fp16_calib_done && !g_fp16_calib_report.reason.empty()) {
            fp16_line = g_fp16_calib_report.reason;
        }
    }

    if (vk_ok) {
        const int def_idx = ncnn::get_default_gpu_index();
        ncnn::VulkanDevice* vkdev = (def_idx >= 0) ? ncnn::get_gpu_device(def_idx) : nullptr;
        const uint32_t vendor_id = (def_idx >= 0) ? ncnn::get_gpu_info(def_idx).vendor_id() : 0u;
        const char* name = (def_idx >= 0) ? ncnn::get_gpu_info(def_idx).device_name() : nullptr;
        const std::string dev_name = (name && name[0] != '\0') ? name : "GPU";
        const uint32_t budget_mb = vkdev ? vkdev->get_heap_budget() : 0u;
        if (fp16_line.empty()) {
            fp16_line = "FP16 storage + FP32 accum default: ON — " + dev_name + " [" + format_hex32(vendor_id) + "]";
        }
        return "Vulkan GPU: " + dev_name + " [" + format_hex32(vendor_id) + "]" +
            " | GPU Heap Budget: " + std::to_string(budget_mb) + " MB" +
            " | create_gpu_instance rc=" + std::to_string(create_rc) +
            " | CPU Cores: " + std::to_string(big_cores) + " big / " + std::to_string(total_cores) + " total\n" +
            fp16_line;
    }

    if (fp16_line.empty()) {
        fp16_line = "FP16 default: N/A — CPU FP32 backend (no Vulkan GPU detected)";
    }
    return "Vulkan unavailable (create_gpu_instance rc=" + std::to_string(create_rc) +
        ", gpu_count=" + std::to_string(g_vulkan_instance_created ? ncnn::get_gpu_count() : 0) + ")" +
        " | CPU Cores: " + std::to_string(big_cores) + " big / " + std::to_string(total_cores) + " total\n" +
        fp16_line;
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

SeamTestReport run_native_seam_test(
    AAssetManager* asset_manager,
    const std::atomic<bool>& cancel_flag,
    const SeamTestProgressCallback& progress_cb
) {
    SeamTestReport report;
    report.fp16_calibration = calibrate_vulkan_fp16_policy(asset_manager);

    constexpr int kInW = 700;
    constexpr int kInH = 500;
    constexpr int kScale = 4;
    constexpr int kOutW = kInW * kScale; // 2800
    constexpr int kOutH = kInH * kScale; // 2000
    constexpr size_t kSrcStride = static_cast<size_t>(kInW) * 4u;
    constexpr size_t kSrcBytes = kSrcStride * static_cast<size_t>(kInH);
    constexpr size_t kOutBytes = static_cast<size_t>(kOutW) * static_cast<size_t>(kOutH) * 3u;

    NativeByteBuffer synth_rgba = allocate_native_buffer(kSrcBytes);
    // Single full-size output RGB buffer reused across every test case (16.8 MB).
    NativeByteBuffer out_rgb = allocate_native_buffer(kOutBytes);
    if (!synth_rgba || !out_rgb) {
        report.status = EnhancerStatus::ERR_OUT_OF_MEMORY;
        report.summary = "FAIL — Out of memory allocating 700x500->2800x2000 Seam Test buffers";
        report.formatted_table = report.summary;
        return report;
    }

    // Generate 700x500 smooth synthetic RGBA test scene:
    //   1) Flat mid-gray studio wall [x < 360, y < 280] (crosses vertical & horizontal splits)
    //   2) Soft smooth wall gradient [x >= 360, y < 280] (tests right-edge smoothness)
    //   3) Bright glossy-floor zone [y >= 280]: flat bright floor [x < 400] + soft specular floor sheen
    //      [x >= 400] (tests bottom-row & right-column glossy floor continuity)
    for (int y = 0; y < kInH; ++y) {
        uint8_t* row = synth_rgba.get() + static_cast<size_t>(y) * kSrcStride;
        for (int x = 0; x < kInW; ++x) {
            uint8_t r = 128u;
            uint8_t g = 128u;
            uint8_t b = 130u;

            if (y < 280) {
                if (x < 360) {
                    r = 128u;
                    g = 128u;
                    b = 130u;
                } else {
                    const float tx = static_cast<float>(x - 360) / 340.0f;
                    const float ty = static_cast<float>(y) / 280.0f;
                    const float v = 128.0f + 56.0f * (tx * tx * (3.0f - 2.0f * tx)) + 16.0f * ty;
                    r = clamp_float_255_to_u8(v);
                    g = clamp_float_255_to_u8(v);
                    b = clamp_float_255_to_u8(v + 2.0f);
                }
            } else {
                if (x < 400) {
                    // Flat bright glossy floor spanning bottom tile split y=333/375 and vertical splits
                    r = 224u;
                    g = 222u;
                    b = 218u;
                } else {
                    // Smooth bright glossy floor highlight near right edge & bottom corner
                    const float tx = static_cast<float>(x - 400) / 300.0f;
                    const float ty = static_cast<float>(y - 280) / 220.0f;
                    const float sheen = 20.0f * (tx * tx * (3.0f - 2.0f * tx)) * (0.5f + 0.5f * ty);
                    r = clamp_float_255_to_u8(224.0f + sheen);
                    g = clamp_float_255_to_u8(222.0f + sheen);
                    b = clamp_float_255_to_u8(218.0f + sheen);
                }
            }

            const size_t x4 = static_cast<size_t>(x) * 4u;
            row[x4 + 0] = r;
            row[x4 + 1] = g;
            row[x4 + 2] = b;
            row[x4 + 3] = 255u;
        }
    }

    // Precompute strict smooth-area mask at input resolution (33x33 source window has zero step)
    // so smooth-area boundary jump evaluation is immune to 1-LSB input quantization terraces.
    std::vector<uint8_t> in_smooth_mask(static_cast<size_t>(kInW * kInH), 0u);
    for (int y = 0; y < kInH; ++y) {
        for (int x = 0; x < kInW; ++x) {
            const int x0 = std::max(0, x - 16);
            const int x1 = std::min(kInW - 1, x + 16);
            const int y0 = std::max(0, y - 16);
            const int y1 = std::min(kInH - 1, y + 16);
            const uint8_t* p0 = synth_rgba.get() + static_cast<size_t>(y) * kSrcStride + static_cast<size_t>(x) * 4u;
            bool flat = true;
            for (int yy = y0; yy <= y1 && flat; ++yy) {
                const uint8_t* rptr = synth_rgba.get() + static_cast<size_t>(yy) * kSrcStride;
                for (int xx = x0; xx <= x1; ++xx) {
                    const uint8_t* q = rptr + static_cast<size_t>(xx) * 4u;
                    if (q[0] != p0[0] || q[1] != p0[1] || q[2] != p0[2]) {
                        flat = false;
                        break;
                    }
                }
            }
            if (flat) {
                in_smooth_mask[static_cast<size_t>(y * kInW + x)] = 1u;
            }
        }
    }

    struct BackendSpec {
        bool use_vulkan;
        const char* name;
    };
    const BackendSpec kBackends[2] = {
        {true, "Vulkan"},
        {false, "CPU"}
    };
    const bool kFp16Modes[2] = {true, false};
    const bool kMaxQModes[2] = {false, true};
    const int kTileSizes[3] = {128, 200, 256};
    constexpr int kTotalCases = 2 * 2 * 2 * 3; // 24 cases

    std::vector<uint8_t> tile_rgb;
    std::vector<uint8_t> tta_scratch_rgb;
    std::vector<float> plane_r;
    std::vector<float> plane_g;
    std::vector<float> plane_b;
    std::vector<uint8_t> band_emit_rgb;

    // Compact 280x200 subsampled RGB snapshot (168 KB, NOT a full-size buffer) for Vulkan vs CPU drift report
    constexpr int kSnapW = kOutW / 10;
    constexpr int kSnapH = kOutH / 10;
    std::vector<uint8_t> vk_fp16_snap;
    std::vector<uint8_t> vk_fp32_snap;
    std::vector<uint8_t> cpu_fp32_snap;

    int case_idx = 0;

    // Evaluate CPU first so `cpu_fp32_snap` is ready, or run in order and compute snapshot diffs after loop!
    for (const BackendSpec& bspec : kBackends) {
        for (bool fp16_on : kFp16Modes) {
            if (cancel_flag.load(std::memory_order_relaxed)) {
                report.status = EnhancerStatus::CANCELLED;
                report.summary = "Cancelled";
                report.formatted_table = "Seam test cancelled.";
                return report;
            }

            UpscaleConfig cfg;
            cfg.param_asset_path = "models/realesrgan-x4plus.param";
            cfg.bin_asset_path = "models/realesrgan-x4plus.bin";
            cfg.model_scale = kScale;
            cfg.disable_fp16 = !fp16_on;
            cfg.force_cpu = !bspec.use_vulkan;
            cfg.sharpness = 0;

            ScopedModelSession session;
            std::string backend_lbl;
            uint32_t budget_mb = 0;
            FailureInfo init_fail;
            const bool sess_ok = session.init(
                asset_manager, cfg, bspec.use_vulkan, 200, backend_lbl, budget_mb, init_fail
            );

            for (bool max_q : kMaxQModes) {
                for (int tile_sz : kTileSizes) {
                    if (cancel_flag.load(std::memory_order_relaxed)) {
                        report.status = EnhancerStatus::CANCELLED;
                        report.summary = "Cancelled";
                        report.formatted_table = "Seam test cancelled.";
                        return report;
                    }

                    const int cur_case_num = case_idx + 1;
                    const std::string case_label = std::string(bspec.name) +
                        " | fp16=" + (fp16_on ? "ON" : "OFF") +
                        " | MaxQ=" + (max_q ? "ON" : "OFF") +
                        " | tile=" + std::to_string(tile_sz);

                    if (progress_cb) {
                        progress_cb(case_idx, kTotalCases, case_label);
                    }

                    SeamTestCaseRow row;
                    row.backend = bspec.name;
                    row.fp16_enabled = fp16_on;
                    row.max_quality = max_q;
                    row.tile_size = tile_sz;

                    if (!sess_ok) {
                        row.backend_available = false;
                        row.passed = false;
                        row.note = bspec.use_vulkan ? "N/A (no Vulkan GPU)" : ("INIT FAIL: " + init_fail.step);
                        report.rows.push_back(row);
                        ++case_idx;
                        continue;
                    }

                    row.backend_available = true;
                    std::memset(out_rgb.get(), 0, kOutBytes);

                    std::vector<int> x_splits;
                    std::vector<int> y_splits;
                    int fixed_tw = 0;
                    int fixed_th = 0;
                    FailureInfo run_fail;

                    const auto t0 = std::chrono::steady_clock::now();
                    const TilePassOutcome rc = run_feathered_tiled_inference(
                        session,
                        kScale,
                        tile_sz,
                        max_q,
                        /*active_tta=*/false,
                        /*sharpness=*/0,
                        backend_lbl,
                        synth_rgba.get(),
                        /*work_is_rgba=*/true,
                        kInW,
                        kInH,
                        kSrcStride,
                        kOutW,
                        /*needs_post_resize=*/false,
                        out_rgb.get(),
                        nullptr,
                        tile_rgb,
                        tta_scratch_rgb,
                        plane_r,
                        plane_g,
                        plane_b,
                        band_emit_rgb,
                        &x_splits,
                        &y_splits,
                        &fixed_tw,
                        &fixed_th,
                        run_fail,
                        cancel_flag,
                        [&](int done_sub, int tot_sub) {
                            if (progress_cb) {
                                progress_cb(
                                    case_idx,
                                    kTotalCases,
                                    case_label + " (" + std::to_string(done_sub) + "/" + std::to_string(tot_sub) + ")"
                                );
                            }
                        }
                    );
                    const auto t1 = std::chrono::steady_clock::now();
                    row.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                    if (rc == TilePassOutcome::CANCELLED) {
                        report.status = EnhancerStatus::CANCELLED;
                        report.summary = "Cancelled";
                        report.formatted_table = "Seam test cancelled.";
                        return report;
                    }
                    if (rc != TilePassOutcome::OK) {
                        row.passed = false;
                        row.note = "ERR: " + run_fail.step;
                        report.rows.push_back(row);
                        ++case_idx;
                        continue;
                    }

                    // Capture compact 10x10-strided snapshot for tile=200, MaxQ=OFF to quantify Vulkan vs CPU drift
                    if (!max_q && tile_sz == 200) {
                        std::vector<uint8_t>* snap_dst = nullptr;
                        if (bspec.use_vulkan && fp16_on) snap_dst = &vk_fp16_snap;
                        else if (bspec.use_vulkan && !fp16_on) snap_dst = &vk_fp32_snap;
                        else if (!bspec.use_vulkan && !fp16_on) snap_dst = &cpu_fp32_snap;

                        if (snap_dst != nullptr) {
                            snap_dst->resize(static_cast<size_t>(kSnapW * kSnapH * 3));
                            for (int sy = 0; sy < kSnapH; ++sy) {
                                for (int sx = 0; sx < kSnapW; ++sx) {
                                    const size_t src_i = (static_cast<size_t>(sy * 10 + 5) * kOutW +
                                        static_cast<size_t>(sx * 10 + 5)) * 3u;
                                    const size_t dst_i = static_cast<size_t>(sy * kSnapW + sx) * 3u;
                                    (*snap_dst)[dst_i + 0] = out_rgb[src_i + 0];
                                    (*snap_dst)[dst_i + 1] = out_rgb[src_i + 1];
                                    (*snap_dst)[dst_i + 2] = out_rgb[src_i + 2];
                                }
                            }
                        }
                    }

                    // Compute seam metrics on `out_rgb` (2800x2000):
                    //   1) Neighboring tile region mean brightness difference (`neighbor_mean_diff`)
                    //   2) Smooth-area boundary jump vs neighboring non-boundary columns/rows
                    const uint8_t* img = out_rgb.get();
                    auto px_max_abs = [&](int x0, int y0, int x1, int y1) -> int {
                        const uint8_t* p = img + (static_cast<size_t>(y0) * kOutW + static_cast<size_t>(x0)) * 3u;
                        const uint8_t* q = img + (static_cast<size_t>(y1) * kOutW + static_cast<size_t>(x1)) * 3u;
                        const int dr = std::abs(static_cast<int>(p[0]) - static_cast<int>(q[0]));
                        const int dg = std::abs(static_cast<int>(p[1]) - static_cast<int>(q[1]));
                        const int db = std::abs(static_cast<int>(p[2]) - static_cast<int>(q[2]));
                        return std::max({dr, dg, db});
                    };
                    auto px_lum = [&](int x, int y) -> double {
                        const uint8_t* p = img + (static_cast<size_t>(y) * kOutW + static_cast<size_t>(x)) * 3u;
                        return (static_cast<double>(p[0]) + static_cast<double>(p[1]) + static_cast<double>(p[2])) / 3.0;
                    };

                    double bnd_sum = 0.0;
                    size_t bnd_cnt = 0;
                    double int_sum = 0.0;
                    size_t int_cnt = 0;
                    int smooth_bnd_max = 0;
                    int smooth_int_max = 0;
                    double max_nbr_diff = 0.0;

                    const int nx = static_cast<int>(x_splits.size()) - 1;
                    const int ny = static_cast<int>(y_splits.size()) - 1;

                    // Vertical tile boundaries
                    for (int ix = 1; ix < nx; ++ix) {
                        const int bx = x_splits[static_cast<size_t>(ix)] * kScale;
                        if (bx < 40 || bx + 40 >= kOutW) continue;

                        for (int iy = 0; iy < ny; ++iy) {
                            const int y0 = y_splits[static_cast<size_t>(iy)] * kScale;
                            const int y1 = y_splits[static_cast<size_t>(iy + 1)] * kScale;
                            double l_sum = 0.0;
                            double r_sum = 0.0;
                            size_t scnt = 0;
                            for (int y = y0; y < y1; ++y) {
                                for (int dx = 1; dx <= 20; ++dx) {
                                    l_sum += px_lum(bx - dx, y);
                                    r_sum += px_lum(bx + dx - 1, y);
                                    ++scnt;
                                }
                            }
                            if (scnt > 0) {
                                max_nbr_diff = std::max(max_nbr_diff, std::abs(l_sum - r_sum) / static_cast<double>(scnt));
                            }
                        }

                        for (int y = 0; y < kOutH; ++y) {
                            const bool is_smooth = in_smooth_mask[static_cast<size_t>((y / kScale) * kInW + (bx / kScale))] != 0u;
                            if (!is_smooth) continue;

                            const int jb = px_max_abs(bx - 1, y, bx, y);
                            const int j1 = px_max_abs(bx - 21, y, bx - 20, y);
                            const int j2 = px_max_abs(bx + 19, y, bx + 20, y);
                            bnd_sum += static_cast<double>(jb);
                            ++bnd_cnt;
                            int_sum += static_cast<double>(j1 + j2);
                            int_cnt += 2u;
                            smooth_bnd_max = std::max(smooth_bnd_max, jb);
                            smooth_int_max = std::max(smooth_int_max, std::max(j1, j2));
                        }
                    }

                    // Horizontal tile boundaries
                    for (int iy = 1; iy < ny; ++iy) {
                        const int by = y_splits[static_cast<size_t>(iy)] * kScale;
                        if (by < 40 || by + 40 >= kOutH) continue;

                        for (int ix = 0; ix < nx; ++ix) {
                            const int x0 = x_splits[static_cast<size_t>(ix)] * kScale;
                            const int x1 = x_splits[static_cast<size_t>(ix + 1)] * kScale;
                            double t_sum = 0.0;
                            double b_sum = 0.0;
                            size_t scnt = 0;
                            for (int dy = 1; dy <= 20; ++dy) {
                                for (int x = x0; x < x1; ++x) {
                                    t_sum += px_lum(x, by - dy);
                                    b_sum += px_lum(x, by + dy - 1);
                                    ++scnt;
                                }
                            }
                            if (scnt > 0) {
                                max_nbr_diff = std::max(max_nbr_diff, std::abs(t_sum - b_sum) / static_cast<double>(scnt));
                            }
                        }

                        for (int x = 0; x < kOutW; ++x) {
                            const bool is_smooth = in_smooth_mask[static_cast<size_t>((by / kScale) * kInW + (x / kScale))] != 0u;
                            if (!is_smooth) continue;

                            const int jb = px_max_abs(x, by - 1, x, by);
                            const int j1 = px_max_abs(x, by - 21, x, by - 20);
                            const int j2 = px_max_abs(x, by + 19, x, by + 20);
                            bnd_sum += static_cast<double>(jb);
                            ++bnd_cnt;
                            int_sum += static_cast<double>(j1 + j2);
                            int_cnt += 2u;
                            smooth_bnd_max = std::max(smooth_bnd_max, jb);
                            smooth_int_max = std::max(smooth_int_max, std::max(j1, j2));
                        }
                    }

                    row.mean_boundary_jump = (bnd_cnt > 0) ? static_cast<float>(bnd_sum / static_cast<double>(bnd_cnt)) : 0.0f;
                    row.mean_interior_jump = (int_cnt > 0) ? static_cast<float>(int_sum / static_cast<double>(int_cnt)) : 0.0f;
                    row.max_boundary_jump = smooth_bnd_max;
                    row.max_interior_jump = smooth_int_max;
                    row.neighbor_mean_diff = static_cast<float>(max_nbr_diff);

                    // FAIL if smooth-area boundary jump exceeds non-boundary baseline by > 2 gray levels
                    // (or absolute smooth boundary jump > 2, or neighbor tile region drift > 1.0 gray level).
                    const int excess_over_baseline = row.max_boundary_jump - row.max_interior_jump;
                    row.passed = (excess_over_baseline <= 2) &&
                                 (row.max_boundary_jump <= 2) &&
                                 (row.neighbor_mean_diff <= 1.0f);
                    row.note = std::to_string(nx) + "x" + std::to_string(ny) +
                        " (" + std::to_string(fixed_tw) + "x" + std::to_string(fixed_th) + ")";

                    report.rows.push_back(row);
                    ++case_idx;
                    (void)cur_case_num;
                }
            }
            session.release();
        }
    }

    if (progress_cb) {
        progress_cb(kTotalCases, kTotalCases, "Complete");
    }

    // Compare Vulkan vs CPU subsampled snapshots if both ran
    std::string cross_backend_line;
    if (!cpu_fp32_snap.empty() && !vk_fp32_snap.empty() && !vk_fp16_snap.empty()) {
        auto snap_diff = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double& mean_d, int& max_d) {
            double s = 0.0;
            int mx = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                s += static_cast<double>(d);
                mx = std::max(mx, d);
            }
            mean_d = s / static_cast<double>(a.size());
            max_d = mx;
        };
        double m16 = 0.0, m32 = 0.0;
        int mx16 = 0, mx32 = 0;
        snap_diff(vk_fp16_snap, cpu_fp32_snap, m16, mx16);
        snap_diff(vk_fp32_snap, cpu_fp32_snap, m32, mx32);
        cross_backend_line = "Vulkan vs CPU (tile=200, MaxQ=OFF): vk_fp16 diff mean=" +
            format_fixed(m16, 2) + " max=" + std::to_string(mx16) +
            " | vk_fp32 diff mean=" + format_fixed(m32, 2) + " max=" + std::to_string(mx32) + "\n";
    }

    int avail_count = 0;
    int pass_count = 0;
    bool active_default_pass = true;
    const bool def_disable_fp16 = report.fp16_calibration.recommend_disable_fp16;

    for (const SeamTestCaseRow& r : report.rows) {
        if (!r.backend_available) continue;
        ++avail_count;
        if (r.passed) {
            ++pass_count;
        }
        const bool is_default_precision =
            (r.backend == "CPU" && !r.fp16_enabled) ||
            (r.backend == "Vulkan" && (r.fp16_enabled == !def_disable_fp16));
        if (is_default_precision && !r.passed) {
            active_default_pass = false;
        }
    }

    report.overall_pass = (avail_count > 0) && active_default_pass && (pass_count == avail_count);
    report.summary = std::string(active_default_pass ? "Default Policy: PASS" : "Default Policy: FAIL") +
        " | Matrix: " + std::to_string(pass_count) + "/" + std::to_string(avail_count) + " available configs PASS";

    std::string table =
        "=== Enhancer Seam Test (700x500 -> 2800x2000, realesrgan-x4plus) ===\n" +
        query_vulkan_diagnostics_summary() + "\n" +
        cross_backend_line +
        report.summary + "\n\n" +
        "Backend | fp16 | MaxQ | Tile | Status | BndMean/IntMean | BndMax/IntMax | NbrShift | Time   | Grid(Padded)\n" +
        "--------+------+------+------+--------+-----------------+---------------+----------+--------+-----------------\n";

    for (const SeamTestCaseRow& r : report.rows) {
        char line[256];
        if (!r.backend_available) {
            std::snprintf(
                line, sizeof(line),
                "%-7s | %-4s | %-4s | %4d | %-6s | %15s | %13s | %8s | %6s | %s\n",
                r.backend.c_str(),
                r.fp16_enabled ? "ON" : "OFF",
                r.max_quality ? "ON" : "OFF",
                r.tile_size,
                "N/A",
                "-",
                "-",
                "-",
                "-",
                r.note.c_str()
            );
        } else {
            const std::string bm_im = format_fixed(r.mean_boundary_jump, 2) + "/" + format_fixed(r.mean_interior_jump, 2);
            const std::string bx_ix = std::to_string(r.max_boundary_jump) + "/" + std::to_string(r.max_interior_jump);
            const std::string nbr = format_fixed(r.neighbor_mean_diff, 2);
            const std::string tms = std::to_string(r.elapsed_ms) + "ms";
            std::snprintf(
                line, sizeof(line),
                "%-7s | %-4s | %-4s | %4d | %-6s | %15s | %13s | %8s | %6s | %s\n",
                r.backend.c_str(),
                r.fp16_enabled ? "ON" : "OFF",
                r.max_quality ? "ON" : "OFF",
                r.tile_size,
                r.passed ? "PASS" : "FAIL",
                bm_im.c_str(),
                bx_ix.c_str(),
                nbr.c_str(),
                tms.c_str(),
                r.note.c_str()
            );
        }
        table += line;
    }

    report.formatted_table = table;
    LOGI("%s", report.formatted_table.c_str());
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
