#ifndef ENHANCER_UPSCALER_H
#define ENHANCER_UPSCALER_H

#include "enhancer_types.h"
#include "image_io.h"

#include <android/asset_manager.h>
#include <atomic>
#include <functional>
#include <string>

namespace enhancer {

// Queries Vulkan availability and GPU device name (empty string if no Vulkan GPU is available).
std::string query_vulkan_device_info();

// Releases global ncnn Vulkan instance on library unload.
void shutdown_vulkan_instance();

// Executes the full native upscale + resize + encode pipeline:
// 1. Pre-downscales with area/box filter if source_long * m >= target (when work_size < src_size).
// 2. Runs tiled Real-ESRGAN inference via ncnn (Vulkan fp16 by default, auto tile-halving down to 48,
//    automatic CPU fallback if Vulkan is unavailable or fails mid-run).
// 3. Post-upscales with streaming row-by-row Lanczos3 if source_long * m < target.
// 4. Encodes RGB888 to JPEG (q=95) or PNG into `fd_guard` via stb_image_write callback.
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
);

} // namespace enhancer

#endif // ENHANCER_UPSCALER_H
