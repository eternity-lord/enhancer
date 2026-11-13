#ifndef ENHANCER_PHOTO_ENHANCE_OPS_H
#define ENHANCER_PHOTO_ENHANCE_OPS_H

#include <atomic>
#include <cstdint>

namespace enhancer {

// Stage 0A: Let's Enhance "Old Photo" pre-SR restoration in-place on compact work_rgb_buf.
// Recovers faded black/white points, removes aged yellow/magenta paper cast (while preserving
// authentic B&W or warm sepia monochrome photos), and suppresses film grain / dust speckles
// before face detection and neural super-resolution.
bool apply_old_photo_pre_restore(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
);

// Stage 0B: Pre-SR chroma/luma bilateral denoise + 8x8 JPEG block-boundary deblocking in-place.
// Prevents Real-ESRGAN from amplifying 8x8 DCT mosquito noise or sensor chroma blotches.
bool apply_pre_denoise_deblock(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
);

// Stage 4A: Let's Enhance "Ultra" 3-band edge-aware multi-scale detail & local clarity engine.
// Operates in-place on final_rgb using a rolling 17-row float ring buffer (< 2.5 MB at 8K)
// with noise-floor coring and soft-tanh halo suppression.
//   intensity:   [0.0 .. 1.0] overall synthesis/clarity strength
//   detail_size: [0.0 .. 1.0] shifts energy from fine micro-texture (0) -> medium (0.5) -> macro clarity (1)
bool apply_ultra_multiscale_detail(
    uint8_t* rgb,
    int width,
    int height,
    float intensity,
    float detail_size,
    const std::atomic<bool>& cancelled
);

// Stage 4B: Let's Enhance "Light AI" Smart HDR tone, shadow/highlight recovery, adaptive white
// balance, and natural vibrance engine. Uses a Fast Guided Filter illumination decomposition
// (< 4 MB scratch even at 7680x4320 8K) for zero-halo local relighting in-place on final_rgb.
bool apply_light_ai_hdr(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
);

} // namespace enhancer

#endif // ENHANCER_PHOTO_ENHANCE_OPS_H
