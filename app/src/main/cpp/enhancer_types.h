#ifndef ENHANCER_TYPES_H
#define ENHANCER_TYPES_H

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

namespace enhancer {

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
};

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
    bool max_quality = false;      // Full-res inference + 32px feathered tiles + Lanczos3 supersampling
    bool tta_x8 = false;           // 8x flip/rotate Test-Time Augmentation
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
    std::string error_detail;
};

using ProgressCallback = std::function<void(int progress, const std::string& backend, int tile_size)>;

} // namespace enhancer

#endif // ENHANCER_TYPES_H
