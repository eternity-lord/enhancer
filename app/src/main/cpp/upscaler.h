#ifndef ENHANCER_UPSCALER_H
#define ENHANCER_UPSCALER_H

#include "enhancer_types.h"

#include <android/asset_manager.h>
#include <atomic>
#include <functional>
#include <string>

namespace enhancer {

// Thread-safely queries Vulkan GPU availability and device name.
std::string query_vulkan_device_info();

// Executes the full v2.0 native upscale + post-process + streaming resize + encode pipeline:
// - Standard mode: area/box pre-downscale when src*m >= target, 10px prepad, optional TTA x8 + unsharp mask.
// - Max Quality mode: full-res source (never pre-downscaled), 32px reflection prepad + 32px linear feathered
//   overlap blending, optional TTA x8 + per-tile adaptive unsharp mask, and band-by-band streaming Lanczos3
//   supersampling downscale/upscale.
// - Encodes packed RGB888 to 4:4:4 full-chroma JPEG (90..100, default 98) or lossless PNG directly to `config.output_fd`.
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
