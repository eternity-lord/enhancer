#ifndef ENHANCER_TYPES_H
#define ENHANCER_TYPES_H

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>

namespace enhancer {

// Distinct native status codes.
// CRITICAL INVARIANT: `ERR_OUT_OF_MEMORY` (-4) may ONLY be produced by a real `std::bad_alloc`
// or a failed `std::malloc`. Never map a Vulkan or `ncnn` failure to `ERR_OUT_OF_MEMORY`.
enum class EnhancerStatus : int {
    OK = 0,
    CANCELLED = 1,
    ERR_INVALID_ARGS = -1,
    ERR_BITMAP_LOCK = -2,
    ERR_ASSET_LOAD = -3,
    ERR_OUT_OF_MEMORY = -4,
    ERR_INFERENCE = -5,
    ERR_ENCODE_WRITE = -6,
    ERR_EXCEPTION = -7,
    ERR_VULKAN_INIT = -8,
    ERR_VK_ALLOCATOR = -9,
    ERR_NCNN_LOAD_PARAM = -10,
    ERR_NCNN_LOAD_MODEL = -11,
    ERR_NCNN_INPUT = -12,
    ERR_NCNN_EXTRACT = -13,
};

inline const char* status_code_name(EnhancerStatus s) noexcept {
    switch (s) {
        case EnhancerStatus::OK: return "OK";
        case EnhancerStatus::CANCELLED: return "CANCELLED";
        case EnhancerStatus::ERR_INVALID_ARGS: return "ERR_INVALID_ARGS";
        case EnhancerStatus::ERR_BITMAP_LOCK: return "ERR_BITMAP_LOCK";
        case EnhancerStatus::ERR_ASSET_LOAD: return "ERR_ASSET_LOAD";
        case EnhancerStatus::ERR_OUT_OF_MEMORY: return "ERR_OUT_OF_MEMORY";
        case EnhancerStatus::ERR_INFERENCE: return "ERR_INFERENCE";
        case EnhancerStatus::ERR_ENCODE_WRITE: return "ERR_ENCODE_WRITE";
        case EnhancerStatus::ERR_EXCEPTION: return "ERR_EXCEPTION";
        case EnhancerStatus::ERR_VULKAN_INIT: return "ERR_VULKAN_INIT";
        case EnhancerStatus::ERR_VK_ALLOCATOR: return "ERR_VK_ALLOCATOR";
        case EnhancerStatus::ERR_NCNN_LOAD_PARAM: return "ERR_NCNN_LOAD_PARAM";
        case EnhancerStatus::ERR_NCNN_LOAD_MODEL: return "ERR_NCNN_LOAD_MODEL";
        case EnhancerStatus::ERR_NCNN_INPUT: return "ERR_NCNN_INPUT";
        case EnhancerStatus::ERR_NCNN_EXTRACT: return "ERR_NCNN_EXTRACT";
    }
    return "ERR_UNKNOWN";
}

// Formats a complete native diagnostic line containing:
// error code + symbolic name, failed step, requested size in bytes, tile size, backend, and ncnn rc.
inline std::string format_step_diagnostic(
    EnhancerStatus status,
    const std::string& step,
    uint64_t requested_bytes,
    int tile_size,
    const std::string& backend,
    int ncnn_rc,
    const std::string& extra = std::string()
) {
    std::string msg = "[code=" + std::to_string(static_cast<int>(status)) + " " +
        status_code_name(status) + "] " +
        "step=" + step +
        " | requested_bytes=" + std::to_string(requested_bytes) +
        " | tile=" + std::to_string(tile_size) +
        " | backend=" + (backend.empty() ? std::string("none") : backend) +
        " | ncnn_rc=" + std::to_string(ncnn_rc);
    if (!extra.empty()) {
        msg += " | detail=" + extra;
    }
    return msg;
}

// Overflow-safe 64-bit / size_t multiplication for `w * h * channels` and buffer byte counts.
inline bool checked_mul_size(size_t a, size_t b, size_t& out) noexcept {
    return !__builtin_mul_overflow(a, b, &out);
}

inline bool checked_mul3_size(size_t a, size_t b, size_t c, size_t& out) noexcept {
    size_t ab = 0;
    if (__builtin_mul_overflow(a, b, &ab)) {
        out = 0;
        return false;
    }
    if (__builtin_mul_overflow(ab, c, &out)) {
        out = 0;
        return false;
    }
    return true;
}

inline bool checked_image_bytes(int w, int h, int channels_or_elem_bytes, size_t& out_bytes) noexcept {
    if (w <= 0 || h <= 0 || channels_or_elem_bytes <= 0) {
        out_bytes = 0;
        return false;
    }
    return checked_mul3_size(
        static_cast<size_t>(w),
        static_cast<size_t>(h),
        static_cast<size_t>(channels_or_elem_bytes),
        out_bytes
    );
}

enum class ImageFormat : int {
    JPEG = 0,
    PNG = 1,
};

struct FreeDeleter {
    void operator()(uint8_t* ptr) const noexcept {
        std::free(ptr);
    }
};

using NativeByteBuffer = std::unique_ptr<uint8_t[], FreeDeleter>;

inline NativeByteBuffer allocate_native_buffer(size_t num_bytes) noexcept {
    if (num_bytes == 0) {
        return NativeByteBuffer(nullptr);
    }
    void* raw = std::malloc(num_bytes);
    return NativeByteBuffer(static_cast<uint8_t*>(raw));
}

struct DimensionPlan {
    int src_w = 0;
    int src_h = 0;
    int work_w = 0;
    int work_h = 0;
    int model_out_w = 0;
    int model_out_h = 0;
    int final_w = 0;
    int final_h = 0;
    bool pre_downscaled = false;
    bool post_upscaled = false;
    bool post_downscaled = false;

    bool needs_post_resize() const noexcept {
        return post_upscaled || post_downscaled;
    }
};

struct UpscaleConfig {
    std::string param_asset_path;
    std::string bin_asset_path;
    int model_scale = 4;
    int target_long_side = 0;      // 0 = Native xN, 3840 = 4K, 7680 = 8K
    int requested_tile_size = 200; // <= 0 means default 200; minimum 48
    bool disable_fp16 = false;
    bool max_quality = false;      // Default OFF: full-res inference + 32px feathered tiles + Lanczos3
    bool tta_x8 = false;           // Default OFF: 8x flip/rotate Test-Time Augmentation
    bool safe_mode = false;        // Default OFF: forces CPU + tile 64 + no Max quality + no TTA
    bool force_cpu = false;        // Forces CPU backend without overriding tile size unless safe_mode
    int sharpness = 0;             // 0..100 adaptive halo-clamped unsharp mask
    ImageFormat format = ImageFormat::JPEG;
    int jpeg_quality = 98;         // 90..100 (default 98), always 4:4:4 full chroma
    int output_fd = -1;
};

struct UpscaleResult {
    EnhancerStatus status = EnhancerStatus::OK;
    int out_width = 0;
    int out_height = 0;
    std::string backend_used;
    bool used_cpu_fallback = false;
    int final_tile_size = 200;
    std::string fallback_summary;  // Ordered human-readable chain of fallbacks used (empty if none)
    std::string error_detail;      // Full formatted diagnostic string on error or trace summary on OK
};

struct BackendSelfTestOutcome {
    bool passed = false;
    int64_t elapsed_ms = 0;
    EnhancerStatus status = EnhancerStatus::OK;
    std::string detail;
};

struct SelfTestReport {
    BackendSelfTestOutcome vulkan;
    BackendSelfTestOutcome cpu;
    std::string full_log;
};

using ProgressCallback = std::function<void(int progress, const std::string& backend, int tile_size)>;

} // namespace enhancer

#endif // ENHANCER_TYPES_H
