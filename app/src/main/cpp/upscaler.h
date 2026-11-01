#ifndef ENHANCER_UPSCALER_H
#define ENHANCER_UPSCALER_H

#include "enhancer_types.h"

#include <android/asset_manager.h>
#include <atomic>
#include <functional>
#include <string>

namespace enhancer {

// Lazily initializes Vulkan GPU instance once and returns GPU device name (or "CPU only").
std::string query_vulkan_device_info();

// Returns a multi-field diagnostic summary of Vulkan GPU device, heap budget (MB),
// create_gpu_instance rc, and CPU big/total thread counts for the About -> Diagnostics screen.
std::string query_vulkan_diagnostics_summary();

// Balanced teardown for ncnn::destroy_gpu_instance(), called from JNI_OnUnload.
void shutdown_vulkan_runtime() noexcept;

// Runs a synthetic 256x256 RGBA image through `models/realesrgan-x4plus.{param,bin}`
// once strictly on Vulkan and once strictly on CPU (`tile = 64`, no Max quality, no TTA),
// recording PASS/FAIL, exact millisecond timings, and full native diagnostic text for each.
SelfTestReport run_native_self_test(AAssetManager* asset_manager);

// Executes the full native upscale + post-process + streaming resize + encode pipeline:
// - Standard mode: area/box pre-downscale when src*m >= target, 10px prepad, optional TTA x8 + unsharp mask.
// - Max Quality mode (off by default): full-res source, 32px reflection prepad + 32px fixed-point feathered
//   overlap blending, optional TTA x8 + per-tile adaptive unsharp mask, and streaming Lanczos3 supersampling.
// - Safe mode (off by default): forces CPU + tile 64 + no Max quality + no TTA.
// - Ordered automatic fallback on failure: halve tile (down to 48) -> disable TTA -> disable Max quality -> CPU.
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
);

} // namespace enhancer

#endif // ENHANCER_UPSCALER_H
