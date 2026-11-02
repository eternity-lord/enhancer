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

// Builds balanced grid split coordinates [0 = splits[0] < splits[1] < ... < splits[N] = total_len]
// with max cell size <= `tile_step` (when `tile_step >= min_cell_len`), distributing cells evenly so
// right-edge and bottom-edge tiles never become narrow runt slivers.
std::vector<int> build_grid_splits(int total_len, int tile_step, int min_cell_len);

// Computes 1D linear tent weight in [0, 1] for pixel `p` belonging to segment `[seg0, seg1)`
// with symmetric feather half-widths `half_left` around `seg0` and `half_right` around `seg1`.
// Integer index offset is computed BEFORE float conversion so even 8K+ coordinates suffer zero
// float subtraction cancellation and adjacent weights satisfy w_{i-1}(p) + w_i(p) == 1.0f.
inline float linear_tent_weight(
    int p,
    int seg0,
    int seg1,
    int half_left,
    int half_right
) noexcept {
    if (half_left > 0 && p < seg0 + half_left) {
        const int idx = p - (seg0 - half_left);
        return static_cast<float>(2 * idx + 1) / static_cast<float>(4 * half_left);
    }
    if (half_right > 0 && p >= seg1 - half_right) {
        const int idx = p - (seg1 - half_right);
        return 1.0f - (static_cast<float>(2 * idx + 1) / static_cast<float>(4 * half_right));
    }
    return 1.0f;
}

} // namespace enhancer

#endif // ENHANCER_TILE_OPS_H
