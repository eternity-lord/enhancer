#include "tile_ops.h"
#include "enhancer_types.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace enhancer {

void d4_transform_rgb(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    int transform_id,
    std::vector<uint8_t>& dst_rgb,
    int& out_w,
    int& out_h
) {
    const int t = transform_id & 7;
    const bool transpose = (t & 4) != 0;
    const bool flip_x = (t & 1) != 0;
    const bool flip_y = (t & 2) != 0;

    out_w = transpose ? src_h : src_w;
    out_h = transpose ? src_w : src_h;

    size_t total_bytes = 0;
    if (!checked_image_bytes(out_w, out_h, 3, total_bytes)) {
        throw std::bad_alloc();
    }
    if (dst_rgb.size() < total_bytes) {
        dst_rgb.resize(total_bytes);
    }

    if (t == 0) {
        std::copy(src_rgb, src_rgb + total_bytes, dst_rgb.begin());
        return;
    }

    for (int sy = 0; sy < src_h; ++sy) {
        const uint8_t* srow = src_rgb + static_cast<size_t>(sy) * static_cast<size_t>(src_w) * 3;
        for (int sx = 0; sx < src_w; ++sx) {
            int dx = transpose ? sy : sx;
            int dy = transpose ? sx : sy;
            if (flip_x) {
                dx = out_w - 1 - dx;
            }
            if (flip_y) {
                dy = out_h - 1 - dy;
            }
            uint8_t* dpx = dst_rgb.data() +
                (static_cast<size_t>(dy) * static_cast<size_t>(out_w) + static_cast<size_t>(dx)) * 3;
            const uint8_t* spx = srow + static_cast<size_t>(sx) * 3;
            dpx[0] = spx[0];
            dpx[1] = spx[1];
            dpx[2] = spx[2];
        }
    }
}

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
) {
    const int t = transform_id & 7;
    const bool transpose = (t & 4) != 0;
    const bool flip_x = (t & 1) != 0;
    const bool flip_y = (t & 2) != 0;

    if (t == 0) {
        #pragma omp parallel for schedule(static)
        for (int oy = 0; oy < base_h; ++oy) {
            const float* sr = trans_r + static_cast<size_t>(oy) * trans_row_stride_floats;
            const float* sg = trans_g + static_cast<size_t>(oy) * trans_row_stride_floats;
            const float* sb = trans_b + static_cast<size_t>(oy) * trans_row_stride_floats;
            float* dr = acc_r + static_cast<size_t>(oy) * static_cast<size_t>(base_w);
            float* dg = acc_g + static_cast<size_t>(oy) * static_cast<size_t>(base_w);
            float* db = acc_b + static_cast<size_t>(oy) * static_cast<size_t>(base_w);
            for (int ox = 0; ox < base_w; ++ox) {
                dr[ox] += sr[ox] * scale_factor;
                dg[ox] += sg[ox] * scale_factor;
                db[ox] += sb[ox] * scale_factor;
            }
        }
        return;
    }

    #pragma omp parallel for schedule(static)
    for (int oy = 0; oy < base_h; ++oy) {
        float* dr = acc_r + static_cast<size_t>(oy) * static_cast<size_t>(base_w);
        float* dg = acc_g + static_cast<size_t>(oy) * static_cast<size_t>(base_w);
        float* db = acc_b + static_cast<size_t>(oy) * static_cast<size_t>(base_w);

        for (int ox = 0; ox < base_w; ++ox) {
            int tx = transpose ? oy : ox;
            int ty = transpose ? ox : oy;
            if (flip_x) {
                tx = trans_w - 1 - tx;
            }
            if (flip_y) {
                ty = trans_h - 1 - ty;
            }
            const size_t tidx =
                static_cast<size_t>(ty) * trans_row_stride_floats + static_cast<size_t>(tx);
            dr[ox] += trans_r[tidx] * scale_factor;
            dg[ox] += trans_g[tidx] * scale_factor;
            db[ox] += trans_b[tidx] * scale_factor;
        }
    }
}

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
) {
    const int clamped_strength = std::clamp(strength_0_to_100, 0, 100);
    if (clamped_strength == 0 || !plane_r || !plane_g || !plane_b) {
        return;
    }

    roi_x0 = std::clamp(roi_x0, 0, plane_w);
    roi_y0 = std::clamp(roi_y0, 0, plane_h);
    roi_x1 = std::clamp(roi_x1, roi_x0, plane_w);
    roi_y1 = std::clamp(roi_y1, roi_y0, plane_h);

    const int roi_w = roi_x1 - roi_x0;
    const int roi_h = roi_y1 - roi_y0;
    if (roi_w <= 0 || roi_h <= 0) {
        return;
    }

    // 5-tap 1D Gaussian kernel for sigma = 1.0 (sum = 1.0)
    constexpr float kG0 = 0.06136f;
    constexpr float kG1 = 0.24477f;
    constexpr float kG2 = 0.38774f;

    const float norm_strength = static_cast<float>(clamped_strength) / 100.0f;
    const float amount = norm_strength * 1.15f;
    // Allow tiny controlled overshoot (<= 2.0/255 at strength 100) while strictly preventing bright/dark halos
    const float max_overshoot = 2.0f * norm_strength;
    // Noise-floor coring threshold in 0..255 space
    constexpr float kNoiseFloor = 1.25f;

    // Horizontal blur needs 2 extra rows above and below ROI for the vertical 5-tap pass
    const int ext_y0 = roi_y0 - 2;
    const int ext_h = roi_h + 4;
    size_t hblur_elems = 0;
    size_t sharp_elems = 0;
    if (!checked_mul_size(static_cast<size_t>(ext_h), static_cast<size_t>(roi_w), hblur_elems) ||
        !checked_mul_size(static_cast<size_t>(roi_h), static_cast<size_t>(roi_w), sharp_elems)) {
        throw std::bad_alloc();
    }
    std::vector<float> hblur(hblur_elems);
    std::vector<float> sharp_out(sharp_elems);

    float* planes[3] = {plane_r, plane_g, plane_b};

    for (int c = 0; c < 3; ++c) {
        float* pl = planes[c];

        // Pass 1: Horizontal 5-tap Gaussian blur across [roi_x0..roi_x1) for rows [roi_y0-2..roi_y1+2)
        #pragma omp parallel for schedule(static)
        for (int ey = 0; ey < ext_h; ++ey) {
            const int sy = std::clamp(ext_y0 + ey, 0, plane_h - 1);
            const float* srow = pl + static_cast<size_t>(sy) * static_cast<size_t>(plane_w);
            float* drow = hblur.data() + static_cast<size_t>(ey) * static_cast<size_t>(roi_w);

            for (int rx = 0; rx < roi_w; ++rx) {
                const int x = roi_x0 + rx;
                const int xm2 = std::max(0, x - 2);
                const int xm1 = std::max(0, x - 1);
                const int xp1 = std::min(plane_w - 1, x + 1);
                const int xp2 = std::min(plane_w - 1, x + 2);

                drow[rx] =
                    kG0 * (srow[xm2] + srow[xp2]) +
                    kG1 * (srow[xm1] + srow[xp1]) +
                    kG2 * srow[x];
            }
        }

        // Pass 2: Vertical 5-tap Gaussian + adaptive edge/noise weighting + 3x3 local min/max halo clamp
        #pragma omp parallel for schedule(static)
        for (int ry = 0; ry < roi_h; ++ry) {
            const int y = roi_y0 + ry;
            const int ym1 = std::max(0, y - 1);
            const int yp1 = std::min(plane_h - 1, y + 1);

            const float* src_top = pl + static_cast<size_t>(ym1) * static_cast<size_t>(plane_w);
            const float* src_mid = pl + static_cast<size_t>(y) * static_cast<size_t>(plane_w);
            const float* src_bot = pl + static_cast<size_t>(yp1) * static_cast<size_t>(plane_w);

            const float* hb0 = hblur.data() + static_cast<size_t>(ry + 0) * static_cast<size_t>(roi_w);
            const float* hb1 = hblur.data() + static_cast<size_t>(ry + 1) * static_cast<size_t>(roi_w);
            const float* hb2 = hblur.data() + static_cast<size_t>(ry + 2) * static_cast<size_t>(roi_w);
            const float* hb3 = hblur.data() + static_cast<size_t>(ry + 3) * static_cast<size_t>(roi_w);
            const float* hb4 = hblur.data() + static_cast<size_t>(ry + 4) * static_cast<size_t>(roi_w);

            float* out_row = sharp_out.data() + static_cast<size_t>(ry) * static_cast<size_t>(roi_w);

            for (int rx = 0; rx < roi_w; ++rx) {
                const int x = roi_x0 + rx;
                const int xm1 = std::max(0, x - 1);
                const int xp1 = std::min(plane_w - 1, x + 1);

                const float center = src_mid[x];
                const float blurred = kG0 * (hb0[rx] + hb4[rx]) + kG1 * (hb1[rx] + hb3[rx]) + kG2 * hb2[rx];
                const float diff = center - blurred;
                const float abs_diff = std::abs(diff);

                if (abs_diff <= kNoiseFloor) {
                    out_row[rx] = center;
                    continue;
                }

                // Smoothly attenuate high-contrast step edges to avoid unnatural ringing
                const float edge_damp = 1.0f / (1.0f + (abs_diff / 36.0f) * (abs_diff / 36.0f));
                const float cored = (diff > 0.0f) ? (diff - kNoiseFloor) : (diff + kNoiseFloor);
                const float candidate = center + amount * edge_damp * cored;

                // 3x3 neighborhood min/max clamp to guarantee halo-free edges
                float lmin = src_top[xm1];
                float lmax = lmin;
                const float nbr[8] = {
                    src_top[x], src_top[xp1],
                    src_mid[xm1], center, src_mid[xp1],
                    src_bot[xm1], src_bot[x], src_bot[xp1]
                };
                for (int i = 0; i < 8; ++i) {
                    lmin = std::min(lmin, nbr[i]);
                    lmax = std::max(lmax, nbr[i]);
                }

                const float clamped = std::clamp(candidate, lmin - max_overshoot, lmax + max_overshoot);
                out_row[rx] = std::clamp(clamped, 0.0f, 255.0f);
            }
        }

        // Copy sharpened ROI back into plane
        for (int ry = 0; ry < roi_h; ++ry) {
            float* dst_row = pl + static_cast<size_t>(roi_y0 + ry) * static_cast<size_t>(plane_w) + roi_x0;
            const float* srow = sharp_out.data() + static_cast<size_t>(ry) * static_cast<size_t>(roi_w);
            std::copy(srow, srow + roi_w, dst_row);
        }
    }
}

std::vector<int> build_grid_splits(int total_len, int tile_step, int min_cell_len) {
    if (total_len <= 0) {
        return {0};
    }
    tile_step = std::max(1, tile_step);
    min_cell_len = std::max(1, min_cell_len);

    if (total_len <= tile_step || total_len < 2 * min_cell_len) {
        return {0, total_len};
    }

    std::vector<int> splits;
    for (int pos = 0; pos < total_len; pos += tile_step) {
        splits.push_back(pos);
    }
    splits.push_back(total_len);

    // If the final cell is narrower than `min_cell_len`, rebalance the last two cells evenly
    if (splits.size() >= 3) {
        const size_t n = splits.size();
        const int last_len = splits[n - 1] - splits[n - 2];
        if (last_len < min_cell_len) {
            const int two_len = splits[n - 1] - splits[n - 3];
            splits[n - 2] = splits[n - 3] + two_len / 2;
        }
    }

    return splits;
}

} // namespace enhancer
