#ifndef ENHANCER_RESIZER_H
#define ENHANCER_RESIZER_H

#include "enhancer_types.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace enhancer {

// Computes exact intermediate and final dimensions while preserving aspect ratio.
DimensionPlan compute_dimensions(int src_w, int src_h, int model_scale, int target_long_side);

// Exact pixel-coverage area/box filter downscaling RGBA_8888 (alpha ignored) to packed RGB888.
// Returns false only if cancelled or arguments are invalid.
bool downscale_rgba_to_rgb_box(
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag
);

// Streaming row-by-row separable Lanczos3 upscaler (radius a = 3).
// Uses a small 8-row rolling float cache (~737 KB even at 8K) instead of a full-image float buffer.
// Returns false if cancelled or on allocation failure.
bool upscale_rgb_lanczos3_streaming(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pct_0_to_100)>& progress_cb = nullptr
);

} // namespace enhancer

#endif // ENHANCER_RESIZER_H
