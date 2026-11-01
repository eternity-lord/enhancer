#ifndef ENHANCER_TILE_OPS_H
#define ENHANCER_TILE_OPS_H

#include <cstdint>
#include <functional>
#include <vector>

namespace enhancer {

// Dihedral group D4 transform `t` in [0..7] on packed RGB888 buffer:
//   bit 2 (t & 4): transpose (x, y) -> (y, x), swapping width & height
//   bit 0 (t & 1): flip X
//   bit 1 (t & 2): flip Y
void d4_transform_rgb(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    int transform_id,
    std::vector<uint8_t>& dst_rgb,
    int& out_w,
    int& out_h
);

// Inverts D4 transform `t` from a transformed planar float RGB output (`trans_w` x `trans_h`,
// values in 0..1) and accumulates into untransformed planar float RGB (`base_w` x `base_h`)
// scaled to 0..255 (`scale_factor = 255.0f / num_passes`, e.g. 255.0f for 1 pass or 255.0f/8.0f for TTA x8).
void d4_inverse_accumulate_planar(
    const float* trans_r,
    const float* trans_g,
    const float* trans_b,
    size_t trans_row_stride_floats,
    int trans_w,
    int trans_h,
    int transform_id,
    float scale_factor,
    float* acc_r,
    float* acc_g,
    float* acc_b,
    int base_w,
    int base_h
);

// Adaptive halo-clamped unsharp mask (radius ~1.0, sigma = 1.0, strength 0..100).
// Operates in-place on planar float RGB (0..255 range) within ROI `[roi_x0, roi_x1) x [roi_y0, roi_y1)`,
// safely reading a 2-pixel neighbor margin from the surrounding CNN tile padding BEFORE blending/cropping.
void apply_adaptive_unsharp_mask_planar(
    float* plane_r,
    float* plane_g,
    float* plane_b,
    int plane_w,
    int plane_h,
    int roi_x0,
    int roi_y0,
    int roi_x1,
    int roi_y1,
    int strength_0_to_100
);

// Builds grid split coordinates [0 = splits[0] < splits[1] < ... < splits[N] = total_len]
// with nominal step `tile_step`, ensuring no trailing sliver is smaller than `min_cell_len`
// whenever `total_len >= 2 * min_cell_len` (so 32px output feathering always has full support).
std::vector<int> build_grid_splits(int total_len, int tile_step, int min_cell_len);

// Computes 1D linear tent weight in [0, 1] for pixel `p` belonging to segment `[seg0, seg1)`
// with symmetric feather half-widths `half_left` around `seg0` and `half_right` around `seg1`.
// Guarantees exact partition of unity: w_{i-1}(p) + w_i(p) == 1.0 across every interior seam.
inline float linear_tent_weight(
    int p,
    int seg0,
    int seg1,
    int half_left,
    int half_right
) noexcept {
    if (half_left > 0 && p < seg0 + half_left) {
        const float start = static_cast<float>(seg0 - half_left);
        const float width = static_cast<float>(2 * half_left);
        return ((static_cast<float>(p) + 0.5f) - start) / width;
    }
    if (half_right > 0 && p >= seg1 - half_right) {
        const float start = static_cast<float>(seg1 - half_right);
        const float width = static_cast<float>(2 * half_right);
        return 1.0f - (((static_cast<float>(p) + 0.5f) - start) / width);
    }
    return 1.0f;
}

} // namespace enhancer

#endif // ENHANCER_TILE_OPS_H
