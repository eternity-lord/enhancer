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

// Synchronous hardware check (e.g., Qualcomm Adreno vendor_id == 0x5143) for initial fp16 default.
bool should_default_disable_fp16_sync() noexcept;

// Runs (once, cached per process) a synthetic smooth-bright-floor tile calibration comparing
// Vulkan (fp16 ON & OFF across tile sizes) against CPU FP32, recording the reason in Diagnostics.
Fp16CalibrationReport calibrate_vulkan_fp16_policy(AAssetManager* asset_manager);

// Runs a synthetic 256x256 RGBA image through `models/realesrgan-x4plus.{param,bin}`
// once strictly on Vulkan and once strictly on CPU (`tile = 64`, no Max quality, no TTA),
// recording PASS/FAIL, exact millisecond timings, and full native diagnostic text for each.
SelfTestReport run_native_self_test(AAssetManager* asset_manager);

// Runs a 700x500 smooth synthetic image (flat gray + soft gradient + bright glossy floor)
// through the exact tiled pipeline on Vulkan and CPU across fp16 ON/OFF, Max quality ON/OFF,
// and tile sizes 128 / 200 / 256, measuring neighboring-tile mean brightness differences and
// max boundary vs interior pixel jumps (FAIL if smooth-area boundary jump > 2 levels).
SeamTestReport run_native_seam_test(
    AAssetManager* asset_manager,
    const std::atomic<bool>& cancel_flag,
    const SeamTestProgressCallback& progress_cb = nullptr
);

// Executes the full native upscale + post-process + streaming resize + encode pipeline:
// - Standard mode: area/box pre-downscale when src*m >= target, >=16px reflection prepad + per-tile
//   float overlap feathering, optional TTA x8 + unsharp mask.
// - Max Quality mode (off by default): full-res source, 32px reflection prepad + 32px per-tile float
//   feathered overlap blending, optional TTA x8 + per-tile adaptive unsharp mask, and streaming Lanczos3.
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
