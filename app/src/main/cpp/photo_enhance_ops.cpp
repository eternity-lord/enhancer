#include "photo_enhance_ops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace enhancer {

namespace {

inline uint8_t clamp_u8(float v) {
    const int iv = static_cast<int>(v + 0.5f);
    return static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

inline float luma_u8(const uint8_t* px) {
    return 0.299f * static_cast<float>(px[0]) +
           0.587f * static_cast<float>(px[1]) +
           0.114f * static_cast<float>(px[2]);
}

inline float median9(float v[9]) {
    auto swap_if = [](float& a, float& b) {
        if (a > b) {
            const float t = a;
            a = b;
            b = t;
        }
    };
    // Partial sorting network for exact median of 9 elements (19 comparisons, branch-light)
    swap_if(v[1], v[2]); swap_if(v[4], v[5]); swap_if(v[7], v[8]);
    swap_if(v[0], v[1]); swap_if(v[3], v[4]); swap_if(v[6], v[7]);
    swap_if(v[1], v[2]); swap_if(v[4], v[5]); swap_if(v[7], v[8]);
    swap_if(v[0], v[3]); swap_if(v[5], v[8]); swap_if(v[4], v[7]);
    swap_if(v[3], v[6]); swap_if(v[1], v[4]); swap_if(v[2], v[5]);
    swap_if(v[4], v[7]); swap_if(v[4], v[2]); swap_if(v[6], v[4]);
    swap_if(v[4], v[2]);
    return v[4];
}

// Separable box filter on a compact float grid (used by Fast Guided Filter on <= 640px grid).
void box_blur_2d(
    const float* src,
    float* tmp,
    float* dst,
    int w,
    int h,
    int radius
) {
    if (w <= 0 || h <= 0 || radius <= 0) {
        std::memcpy(dst, src, static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(float));
        return;
    }
    const int r = std::min(radius, std::max(1, std::min(w, h) / 2));

    // Horizontal pass
    #pragma omp parallel for schedule(static) if (h >= 32)
    for (int y = 0; y < h; ++y) {
        const float* srow = src + static_cast<size_t>(y) * w;
        float* trow = tmp + static_cast<size_t>(y) * w;
        double sum = 0.0;
        int count = 0;
        for (int x = 0; x <= std::min(w - 1, r); ++x) {
            sum += srow[x];
            ++count;
        }
        for (int x = 0; x < w; ++x) {
            trow[x] = static_cast<float>(sum / static_cast<double>(count));
            const int remove_x = x - r;
            const int add_x = x + r + 1;
            if (remove_x >= 0) {
                sum -= srow[remove_x];
                --count;
            }
            if (add_x < w) {
                sum += srow[add_x];
                ++count;
            }
        }
    }

    // Vertical pass
    #pragma omp parallel for schedule(static) if (w >= 32)
    for (int x = 0; x < w; ++x) {
        double sum = 0.0;
        int count = 0;
        for (int y = 0; y <= std::min(h - 1, r); ++y) {
            sum += tmp[static_cast<size_t>(y) * w + x];
            ++count;
        }
        for (int y = 0; y < h; ++y) {
            dst[static_cast<size_t>(y) * w + x] =
                static_cast<float>(sum / static_cast<double>(count));
            const int remove_y = y - r;
            const int add_y = y + r + 1;
            if (remove_y >= 0) {
                sum -= tmp[static_cast<size_t>(remove_y) * w + x];
                --count;
            }
            if (add_y < h) {
                sum += tmp[static_cast<size_t>(add_y) * w + x];
                ++count;
            }
        }
    }
}

inline float sample_bilinear(const float* grid, int gw, int gh, float gx, float gy) {
    const float cx = std::clamp(gx, 0.0f, static_cast<float>(gw - 1));
    const float cy = std::clamp(gy, 0.0f, static_cast<float>(gh - 1));
    const int x0 = static_cast<int>(cx);
    const int y0 = static_cast<int>(cy);
    const int x1 = std::min(x0 + 1, gw - 1);
    const int y1 = std::min(y0 + 1, gh - 1);
    const float fx = cx - static_cast<float>(x0);
    const float fy = cy - static_cast<float>(y0);

    const float v00 = grid[static_cast<size_t>(y0) * gw + x0];
    const float v01 = grid[static_cast<size_t>(y0) * gw + x1];
    const float v10 = grid[static_cast<size_t>(y1) * gw + x0];
    const float v11 = grid[static_cast<size_t>(y1) * gw + x1];

    const float top = v00 + fx * (v01 - v00);
    const float bot = v10 + fx * (v11 - v10);
    return top + fy * (bot - top);
}

inline float soft_core_and_limit(float d, float noise_floor, float halo_cap) {
    const float ad = std::fabs(d);
    if (ad <= 1e-6f) {
        return 0.0f;
    }
    // Smooth quadratic coring suppresses flat-sky sensor noise without hard thresholding seams
    const float cored = d * (ad / (ad + noise_floor));
    // Soft-tanh limiter prevents dark/bright halos around high-contrast edges & text
    return halo_cap * std::tanh(cored / halo_cap);
}

} // namespace

bool apply_old_photo_pre_restore(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
) {
    if (!rgb || width <= 2 || height <= 2 || strength <= 0.01f) {
        return true;
    }
    const float s = std::clamp(strength, 0.0f, 1.0f);
    const size_t total_px = static_cast<size_t>(width) * static_cast<size_t>(height);

    // 1. Subsampled scene statistics (luminance histogram + midtone color cast vs monochrome check)
    std::array<uint64_t, 256> hist_y{};
    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    double sum_dr = 0.0, sum_db = 0.0;
    double sum_dr2 = 0.0, sum_db2 = 0.0;
    double sum_sat = 0.0;
    uint64_t mid_count = 0;

    const int step = std::max(1, std::min(width, height) / 512);
    for (int y = 0; y < height; y += step) {
        const uint8_t* row = rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += step) {
            const uint8_t* px = row + static_cast<size_t>(x) * 3u;
            const float r = static_cast<float>(px[0]);
            const float g = static_cast<float>(px[1]);
            const float b = static_cast<float>(px[2]);
            const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
            const int bin = std::clamp(static_cast<int>(lum + 0.5f), 0, 255);
            hist_y[bin]++;

            if (lum > 30.0f && lum < 225.0f) {
                const double dr = static_cast<double>(r - lum);
                const double db = static_cast<double>(b - lum);
                sum_r += r;
                sum_g += g;
                sum_b += b;
                sum_dr += dr;
                sum_db += db;
                sum_dr2 += dr * dr;
                sum_db2 += db * db;
                const float mx = std::max({r, g, b});
                const float mn = std::min({r, g, b});
                sum_sat += (mx > 1.0f) ? ((mx - mn) / mx) : 0.0f;
                ++mid_count;
            }
        }
    }

    if (cancelled.load()) {
        return false;
    }

    uint64_t sampled_total = 0;
    for (uint64_t c : hist_y) sampled_total += c;
    if (sampled_total == 0) return true;

    uint64_t acc = 0;
    int p01 = 0;
    int p99 = 255;
    for (int i = 0; i < 256; ++i) {
        acc += hist_y[i];
        if (acc * 100 >= sampled_total) {
            p01 = i;
            break;
        }
    }
    acc = 0;
    for (int i = 255; i >= 0; --i) {
        acc += hist_y[i];
        if (acc * 100 >= sampled_total) {
            p99 = i;
            break;
        }
    }

    const float black_pt = static_cast<float>(p01) * 0.88f * s;
    const float white_pt = 255.0f - (255.0f - static_cast<float>(p99)) * (0.88f * s);
    const float span = std::max(60.0f, white_pt - black_pt);

    bool is_color_photo = false;
    bool is_neutral_bw = false;
    float wb_r = 1.0f, wb_g = 1.0f, wb_b = 1.0f;
    if (mid_count > 32) {
        const double inv_n = 1.0 / static_cast<double>(mid_count);
        const double mean_dr = sum_dr * inv_n;
        const double mean_db = sum_db * inv_n;
        const double var_dr = std::max(0.0, sum_dr2 * inv_n - mean_dr * mean_dr);
        const double var_db = std::max(0.0, sum_db2 * inv_n - mean_db * mean_db);
        const double chroma_std = std::sqrt(var_dr + var_db);
        const double mean_sat = sum_sat * inv_n;

        is_color_photo = (chroma_std > 8.5);
        is_neutral_bw = (!is_color_photo && mean_sat < 0.08);

        if (is_color_photo) {
            const float mr = static_cast<float>(sum_r * inv_n);
            const float mg = static_cast<float>(sum_g * inv_n);
            const float mb = static_cast<float>(sum_b * inv_n);
            const float m_avg = (mr + mg + mb) / 3.0f;
            if (mr > 8.0f && mg > 8.0f && mb > 8.0f) {
                const float raw_r = std::clamp(m_avg / mr, 0.82f, 1.22f);
                const float raw_g = std::clamp(m_avg / mg, 0.86f, 1.16f);
                const float raw_b = std::clamp(m_avg / mb, 0.82f, 1.25f);
                const float cast_blend = 0.62f * s;
                wb_r = 1.0f + cast_blend * (raw_r - 1.0f);
                wb_g = 1.0f + cast_blend * (raw_g - 1.0f);
                wb_b = 1.0f + cast_blend * (raw_b - 1.0f);
            }
        }
    }

    // 2. Tone dehaze + cast cleanup in-place
    #pragma omp parallel for schedule(static) if (total_px >= 16384)
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + static_cast<size_t>(x) * 3u;
            float r = static_cast<float>(px[0]) * wb_r;
            float g = static_cast<float>(px[1]) * wb_g;
            float b = static_cast<float>(px[2]) * wb_b;

            const float y_in = 0.299f * r + 0.587f * g + 0.114f * b;
            float yn = std::clamp((y_in - black_pt) / span, 0.0f, 1.0f);
            // Gentle vintage S-curve lift for faded silver prints
            const float y_scurve = yn * yn * (3.0f - 2.0f * yn);
            yn = yn + (0.26f * s) * (y_scurve - yn);
            const float y_out = yn * 255.0f;

            const float ratio = (y_in > 1.0f) ? (y_out / y_in) : 1.0f;
            float nr = (y_in > 1.0f) ? (r * ratio) : (r + (y_out - y_in));
            float ng = (y_in > 1.0f) ? (g * ratio) : (g + (y_out - y_in));
            float nb = (y_in > 1.0f) ? (b * ratio) : (b + (y_out - y_in));

            if (is_neutral_bw) {
                // Clean residual scanner color tint on B&W photographs
                const float clean_mix = 0.78f * s;
                nr = nr + clean_mix * (y_out - nr);
                ng = ng + clean_mix * (y_out - ng);
                nb = nb + clean_mix * (y_out - nb);
            } else if (!is_color_photo) {
                // Sepia / monochrome warm archival print: suppress green/magenta scanner noise only
                const float warm_r = y_out + 0.85f * (nr - y_out);
                const float warm_b = y_out + 0.85f * (nb - y_out);
                const float warm_g = 0.587f > 0.0f
                    ? (y_out - 0.299f * warm_r - 0.114f * warm_b) / 0.587f
                    : ng;
                nr = warm_r;
                ng = ng + (0.65f * s) * (warm_g - ng);
                nb = warm_b;
            }

            px[0] = clamp_u8(nr);
            px[1] = clamp_u8(ng);
            px[2] = clamp_u8(nb);
        }
    }

    if (cancelled.load()) {
        return false;
    }

    // 3. Rolling 3-row 3x3 dust/speckle median + edge-preserving grain filter (< 32 KB scratch)
    const size_t row_bytes = static_cast<size_t>(width) * 3u;
    std::vector<uint8_t> ring_buf;
    try {
        ring_buf.resize(row_bytes * 3u);
    } catch (const std::bad_alloc&) {
        return true; // Tonal restoration already succeeded; skip optional scratch pass safely
    }

    auto ring_row = [&](int ry) -> uint8_t* {
        return ring_buf.data() + static_cast<size_t>((ry % 3 + 3) % 3) * row_bytes;
    };

    std::memcpy(ring_row(0), rgb, row_bytes);
    std::memcpy(ring_row(1), rgb + row_bytes, row_bytes);

    const float outlier_thresh = 24.0f - 8.0f * s;
    const float inv_2sigma_r2 = 1.0f / (2.0f * (13.0f * s + 3.0f) * (13.0f * s + 3.0f));

    for (int y = 0; y < height; ++y) {
        if ((y & 63) == 0 && cancelled.load()) {
            return false;
        }
        const int next_y = std::min(y + 1, height - 1);
        std::memcpy(ring_row(y + 1), rgb + static_cast<size_t>(next_y) * row_bytes, row_bytes);

        const uint8_t* r_prev = ring_row(std::max(0, y - 1));
        const uint8_t* r_curr = ring_row(y);
        const uint8_t* r_next = ring_row(next_y);
        uint8_t* dst_row = rgb + static_cast<size_t>(y) * row_bytes;

        for (int x = 0; x < width; ++x) {
            const int xm1 = std::max(0, x - 1);
            const int xp1 = std::min(width - 1, x + 1);

            const uint8_t* p[9] = {
                r_prev + xm1 * 3, r_prev + x * 3, r_prev + xp1 * 3,
                r_curr + xm1 * 3, r_curr + x * 3, r_curr + xp1 * 3,
                r_next + xm1 * 3, r_next + x * 3, r_next + xp1 * 3
            };
            float lums[9];
            for (int k = 0; k < 9; ++k) {
                lums[k] = luma_u8(p[k]);
            }
            const float center_y = lums[4];
            float lums_copy[9];
            std::memcpy(lums_copy, lums, sizeof(lums));
            const float med_y = median9(lums_copy);

            // Isolated silver-halide dust/scratch outlier replacement + gentle bilateral grain clean
            if (std::fabs(center_y - med_y) > outlier_thresh) {
                // Find neighbor closest to median luminance to preserve color fidelity
                int best_k = 4;
                float best_diff = 1e9f;
                for (int k = 0; k < 9; ++k) {
                    const float d = std::fabs(lums[k] - med_y);
                    if (d < best_diff) {
                        best_diff = d;
                        best_k = k;
                    }
                }
                const float mix = 0.85f * s;
                dst_row[x * 3 + 0] = clamp_u8(p[4][0] + mix * (static_cast<float>(p[best_k][0]) - p[4][0]));
                dst_row[x * 3 + 1] = clamp_u8(p[4][1] + mix * (static_cast<float>(p[best_k][1]) - p[4][1]));
                dst_row[x * 3 + 2] = clamp_u8(p[4][2] + mix * (static_cast<float>(p[best_k][2]) - p[4][2]));
            } else {
                static const float kSpatial[9] = {
                    0.5f, 0.75f, 0.5f,
                    0.75f, 1.0f, 0.75f,
                    0.5f, 0.75f, 0.5f
                };
                float wr_sum = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
                for (int k = 0; k < 9; ++k) {
                    const float dy = lums[k] - center_y;
                    const float w = kSpatial[k] * std::exp(-dy * dy * inv_2sigma_r2);
                    wr_sum += w;
                    sr += w * static_cast<float>(p[k][0]);
                    sg += w * static_cast<float>(p[k][1]);
                    sb += w * static_cast<float>(p[k][2]);
                }
                const float inv_w = 1.0f / std::max(1e-5f, wr_sum);
                dst_row[x * 3 + 0] = clamp_u8(sr * inv_w);
                dst_row[x * 3 + 1] = clamp_u8(sg * inv_w);
                dst_row[x * 3 + 2] = clamp_u8(sb * inv_w);
            }
        }
    }

    return true;
}

bool apply_pre_denoise_deblock(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
) {
    if (!rgb || width <= 4 || height <= 4 || strength <= 0.01f) {
        return true;
    }
    const float s = std::clamp(strength, 0.0f, 1.0f);
    const size_t row_bytes = static_cast<size_t>(width) * 3u;

    // Pass A: Rolling 5-row YCbCr bilateral denoise (stronger chroma cleanup, crisp luma edges)
    std::vector<uint8_t> ring_buf;
    try {
        ring_buf.resize(row_bytes * 5u);
    } catch (const std::bad_alloc&) {
        return true;
    }

    auto ring_row = [&](int ry) -> uint8_t* {
        return ring_buf.data() + static_cast<size_t>((ry % 5 + 5) % 5) * row_bytes;
    };

    for (int init_y = 0; init_y < std::min(height, 3); ++init_y) {
        std::memcpy(ring_row(init_y), rgb + static_cast<size_t>(init_y) * row_bytes, row_bytes);
    }

    const float sigma_y = 3.5f + 9.5f * s;
    const float sigma_c = 7.0f + 18.0f * s;
    const float inv_2sy2 = 1.0f / (2.0f * sigma_y * sigma_y);
    const float inv_2sc2 = 1.0f / (2.0f * sigma_c * sigma_c);

    for (int y = 0; y < height; ++y) {
        if ((y & 63) == 0 && cancelled.load()) {
            return false;
        }
        const int fetch_y = std::min(y + 2, height - 1);
        std::memcpy(ring_row(y + 2), rgb + static_cast<size_t>(fetch_y) * row_bytes, row_bytes);

        const uint8_t* rows[5] = {
            ring_row(std::clamp(y - 2, 0, height - 1)),
            ring_row(std::clamp(y - 1, 0, height - 1)),
            ring_row(y),
            ring_row(std::clamp(y + 1, 0, height - 1)),
            ring_row(std::clamp(y + 2, 0, height - 1))
        };
        uint8_t* dst = rgb + static_cast<size_t>(y) * row_bytes;

        #pragma omp parallel for schedule(static) if (width >= 256)
        for (int x = 0; x < width; ++x) {
            const uint8_t* cpx = rows[2] + x * 3;
            const float cr = static_cast<float>(cpx[0]);
            const float cg = static_cast<float>(cpx[1]);
            const float cb_b = static_cast<float>(cpx[2]);
            const float cy = 0.299f * cr + 0.587f * cg + 0.114f * cb_b;
            const float c_cb = -0.168736f * cr - 0.331264f * cg + 0.5f * cb_b;
            const float c_cr = 0.5f * cr - 0.418688f * cg - 0.081312f * cb_b;

            float wy_sum = 0.0f, y_acc = 0.0f;
            float wc_sum = 0.0f, cb_acc = 0.0f, cr_acc = 0.0f;

            for (int dy = -2; dy <= 2; ++dy) {
                const uint8_t* rptr = rows[dy + 2];
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = std::clamp(x + dx, 0, width - 1);
                    const uint8_t* npx = rptr + nx * 3;
                    const float nr = static_cast<float>(npx[0]);
                    const float ng = static_cast<float>(npx[1]);
                    const float nb = static_cast<float>(npx[2]);
                    const float ny = 0.299f * nr + 0.587f * ng + 0.114f * nb;
                    const float n_cb = -0.168736f * nr - 0.331264f * ng + 0.5f * nb;
                    const float n_cr = 0.5f * nr - 0.418688f * ng - 0.081312f * nb;

                    const float dist2 = static_cast<float>(dx * dx + dy * dy);
                    const float w_sp = (dist2 == 0.0f) ? 1.0f : (dist2 <= 2.0f ? 0.72f : 0.38f);
                    const float diff_y = ny - cy;
                    const float diff_c2 = (n_cb - c_cb) * (n_cb - c_cb) + (n_cr - c_cr) * (n_cr - c_cr);

                    const float wy = w_sp * std::exp(-(diff_y * diff_y) * inv_2sy2);
                    const float wc = w_sp * std::exp(-(diff_y * diff_y * 0.5f + diff_c2) * inv_2sc2);

                    wy_sum += wy;
                    y_acc += wy * ny;
                    wc_sum += wc;
                    cb_acc += wc * n_cb;
                    cr_acc += wc * n_cr;
                }
            }

            const float out_y = y_acc / std::max(1e-5f, wy_sum);
            const float out_cb = cb_acc / std::max(1e-5f, wc_sum);
            const float out_cr = cr_acc / std::max(1e-5f, wc_sum);

            dst[x * 3 + 0] = clamp_u8(out_y + 1.402f * out_cr);
            dst[x * 3 + 1] = clamp_u8(out_y - 0.344136f * out_cb - 0.714136f * out_cr);
            dst[x * 3 + 2] = clamp_u8(out_y + 1.772f * out_cb);
        }
    }

    if (cancelled.load()) {
        return false;
    }

    // Pass B: ITU-style 8x8 JPEG block-boundary deblocking (relaxes flat-interior 8x8 steps only)
    const float alpha = 6.0f + 16.0f * s;
    const float beta = 3.0f + 8.0f * s;

    // Vertical 8-pixel boundaries
    #pragma omp parallel for schedule(static) if (height >= 64)
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * row_bytes;
        for (int bx = 8; bx <= width - 2; bx += 8) {
            for (int c = 0; c < 3; ++c) {
                const float p1 = static_cast<float>(row[(bx - 2) * 3 + c]);
                const float p0 = static_cast<float>(row[(bx - 1) * 3 + c]);
                const float q0 = static_cast<float>(row[bx * 3 + c]);
                const float q1 = static_cast<float>(row[(bx + 1) * 3 + c]);
                if (std::fabs(p0 - q0) < alpha &&
                    std::fabs(p1 - p0) < beta &&
                    std::fabs(q1 - q0) < beta) {
                    const float delta = std::clamp(
                        0.38f * s * ((q0 - p0) + 0.25f * (p1 - q1)),
                        -6.0f * s,
                        6.0f * s
                    );
                    row[(bx - 1) * 3 + c] = clamp_u8(p0 + delta);
                    row[bx * 3 + c] = clamp_u8(q0 - delta);
                }
            }
        }
    }

    // Horizontal 8-pixel boundaries
    for (int by = 8; by <= height - 2; by += 8) {
        uint8_t* r_p1 = rgb + static_cast<size_t>(by - 2) * row_bytes;
        uint8_t* r_p0 = rgb + static_cast<size_t>(by - 1) * row_bytes;
        uint8_t* r_q0 = rgb + static_cast<size_t>(by) * row_bytes;
        uint8_t* r_q1 = rgb + static_cast<size_t>(by + 1) * row_bytes;
        #pragma omp parallel for schedule(static) if (width >= 128)
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float p1 = static_cast<float>(r_p1[x * 3 + c]);
                const float p0 = static_cast<float>(r_p0[x * 3 + c]);
                const float q0 = static_cast<float>(r_q0[x * 3 + c]);
                const float q1 = static_cast<float>(r_q1[x * 3 + c]);
                if (std::fabs(p0 - q0) < alpha &&
                    std::fabs(p1 - p0) < beta &&
                    std::fabs(q1 - q0) < beta) {
                    const float delta = std::clamp(
                        0.38f * s * ((q0 - p0) + 0.25f * (p1 - q1)),
                        -6.0f * s,
                        6.0f * s
                    );
                    r_p0[x * 3 + c] = clamp_u8(p0 + delta);
                    r_q0[x * 3 + c] = clamp_u8(q0 - delta);
                }
            }
        }
    }

    return true;
}

bool apply_ultra_multiscale_detail(
    uint8_t* rgb,
    int width,
    int height,
    float intensity,
    float detail_size,
    const std::atomic<bool>& cancelled
) {
    if (!rgb || width <= 4 || height <= 4 || intensity <= 0.01f) {
        return true;
    }

    const float inten = std::clamp(intensity, 0.0f, 1.0f);
    const float sz = std::clamp(detail_size, 0.0f, 1.0f);

    // Continuous 3-band Let's Enhance Ultra detail decomposition:
    //   sz = 0.0 -> crisp micro-detail (pores, eyelashes, fine weave, r=1)
    //   sz = 0.5 -> balanced medium clarity (foliage, hair strands, r=3)
    //   sz = 1.0 -> bold local structure & relief (architecture, rocks, r=7)
    const float w_fine = inten * (1.15f * (1.0f - sz) * (1.0f - sz) + 0.30f);
    const float w_med  = inten * (0.95f * (1.0f - std::fabs(2.0f * sz - 1.0f)) + 0.35f);
    const float w_mac  = inten * (0.85f * sz * sz + 0.15f);

    constexpr int kR0 = 1;
    constexpr int kR1 = 3;
    constexpr int kR2 = 7;
    constexpr int kRingRows = 2 * kR2 + 3; // 17 rows -> < 2.2 MB total scratch even at 7680px 8K!

    std::vector<float> luma_ring;
    std::vector<float> hbox0_ring;
    std::vector<float> hbox1_ring;
    std::vector<float> hbox2_ring;
    try {
        const size_t ring_elems = static_cast<size_t>(kRingRows) * static_cast<size_t>(width);
        luma_ring.resize(ring_elems);
        hbox0_ring.resize(ring_elems);
        hbox1_ring.resize(ring_elems);
        hbox2_ring.resize(ring_elems);
    } catch (const std::bad_alloc&) {
        return true; // Never fail export if scratch cannot allocate
    }

    const size_t row_bytes = static_cast<size_t>(width) * 3u;

    auto compute_row_into_slot = [&](int src_y, int slot) {
        const int clamped_y = std::clamp(src_y, 0, height - 1);
        const uint8_t* srow = rgb + static_cast<size_t>(clamped_y) * row_bytes;
        float* lrow = luma_ring.data() + static_cast<size_t>(slot) * width;
        float* b0 = hbox0_ring.data() + static_cast<size_t>(slot) * width;
        float* b1 = hbox1_ring.data() + static_cast<size_t>(slot) * width;
        float* b2 = hbox2_ring.data() + static_cast<size_t>(slot) * width;

        for (int x = 0; x < width; ++x) {
            lrow[x] = luma_u8(srow + static_cast<size_t>(x) * 3u) * (1.0f / 255.0f);
        }

        auto horiz_box = [&](int r, float* dst) {
            const int rr = std::min(r, std::max(1, width - 1));
            double sum = 0.0;
            for (int k = -rr; k <= rr; ++k) {
                sum += lrow[std::clamp(k, 0, width - 1)];
            }
            const double inv = 1.0 / static_cast<double>(2 * rr + 1);
            for (int x = 0; x < width; ++x) {
                dst[x] = static_cast<float>(sum * inv);
                const int rem = std::clamp(x - rr, 0, width - 1);
                const int add = std::clamp(x + rr + 1, 0, width - 1);
                sum += static_cast<double>(lrow[add]) - static_cast<double>(lrow[rem]);
            }
        };

        horiz_box(kR0, b0);
        horiz_box(kR1, b1);
        horiz_box(kR2, b2);
    };

    // Prime initial ring slots [-kR2 .. kR2 - 1]
    for (int sy = -kR2; sy < kR2; ++sy) {
        const int slot = (sy + kRingRows * 4) % kRingRows;
        compute_row_into_slot(sy, slot);
    }

    const float noise_floor_0 = 0.0055f; // ~1.4 / 255
    const float noise_floor_1 = 0.0045f;
    const float noise_floor_2 = 0.0040f;
    const float halo_cap_0 = 0.055f;     // ~14 / 255 max per-band overshoot
    const float halo_cap_1 = 0.065f;
    const float halo_cap_2 = 0.075f;

    for (int y = 0; y < height; ++y) {
        if ((y & 63) == 0 && cancelled.load()) {
            return false;
        }
        const int new_sy = y + kR2;
        const int new_slot = (new_sy + kRingRows * 4) % kRingRows;
        compute_row_into_slot(new_sy, new_slot);

        const int c_slot = (y + kRingRows * 4) % kRingRows;
        const float* c_luma = luma_ring.data() + static_cast<size_t>(c_slot) * width;
        uint8_t* dst_row = rgb + static_cast<size_t>(y) * row_bytes;

        int slots0[2 * kR0 + 1];
        int slots1[2 * kR1 + 1];
        int slots2[2 * kR2 + 1];
        for (int k = -kR0; k <= kR0; ++k) slots0[k + kR0] = (y + k + kRingRows * 4) % kRingRows;
        for (int k = -kR1; k <= kR1; ++k) slots1[k + kR1] = (y + k + kRingRows * 4) % kRingRows;
        for (int k = -kR2; k <= kR2; ++k) slots2[k + kR2] = (y + k + kRingRows * 4) % kRingRows;

        #pragma omp parallel for schedule(static) if (width >= 256)
        for (int x = 0; x < width; ++x) {
            const float yc = c_luma[x];

            float blur0 = 0.0f;
            for (int k = 0; k < 2 * kR0 + 1; ++k) {
                blur0 += hbox0_ring[static_cast<size_t>(slots0[k]) * width + x];
            }
            blur0 *= (1.0f / static_cast<float>(2 * kR0 + 1));

            float blur1 = 0.0f;
            for (int k = 0; k < 2 * kR1 + 1; ++k) {
                blur1 += hbox1_ring[static_cast<size_t>(slots1[k]) * width + x];
            }
            blur1 *= (1.0f / static_cast<float>(2 * kR1 + 1));

            float blur2 = 0.0f;
            for (int k = 0; k < 2 * kR2 + 1; ++k) {
                blur2 += hbox2_ring[static_cast<size_t>(slots2[k]) * width + x];
            }
            blur2 *= (1.0f / static_cast<float>(2 * kR2 + 1));

            // Multi-scale Laplacian pyramid bands:
            //   band0 = Y - blur(r=1)        (ultra-fine detail)
            //   band1 = blur(r=1) - blur(r=3) (medium detail)
            //   band2 = blur(r=3) - blur(r=7) (local clarity / structure)
            const float d0 = soft_core_and_limit(yc - blur0, noise_floor_0, halo_cap_0);
            const float d1 = soft_core_and_limit(blur0 - blur1, noise_floor_1, halo_cap_1);
            const float d2 = soft_core_and_limit(blur1 - blur2, noise_floor_2, halo_cap_2);

            // Attenuate near extreme highlights/shadows to prevent clipping
            const float tone_guard = 0.25f + 0.75f * (4.0f * yc * (1.0f - yc));
            const float delta = (w_fine * d0 + w_med * d1 + w_mac * d2) * tone_guard * 255.0f;

            uint8_t* px = dst_row + static_cast<size_t>(x) * 3u;
            px[0] = clamp_u8(static_cast<float>(px[0]) + delta);
            px[1] = clamp_u8(static_cast<float>(px[1]) + delta);
            px[2] = clamp_u8(static_cast<float>(px[2]) + delta);
        }
    }

    return true;
}

bool apply_light_ai_hdr(
    uint8_t* rgb,
    int width,
    int height,
    float strength,
    const std::atomic<bool>& cancelled
) {
    if (!rgb || width <= 4 || height <= 4 || strength <= 0.01f) {
        return true;
    }

    const float s = std::clamp(strength, 0.0f, 1.0f);
    const int max_dim = std::max(width, height);

    // Compact Guided-Filter grid <= 512px on longest side (< 4 MB total scratch even at 16K!)
    const int subsample = std::max(2, (max_dim + 511) / 512);
    const int gw = std::max(2, (width + subsample - 1) / subsample);
    const int gh = std::max(2, (height + subsample - 1) / subsample);
    const size_t gn = static_cast<size_t>(gw) * static_cast<size_t>(gh);

    std::vector<float> I_sub;
    std::vector<float> mean_I;
    std::vector<float> II_sub;
    std::vector<float> corr_I;
    std::vector<float> a_grid;
    std::vector<float> b_grid;
    std::vector<float> tmp_grid;
    try {
        I_sub.resize(gn);
        mean_I.resize(gn);
        II_sub.resize(gn);
        corr_I.resize(gn);
        a_grid.resize(gn);
        b_grid.resize(gn);
        tmp_grid.resize(gn);
    } catch (const std::bad_alloc&) {
        return true;
    }

    // 1. Area-average downsample luminance onto I_sub + gather scene exposure & color stats
    std::array<uint64_t, 256> hist_y{};
    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    double sum_dr = 0.0, sum_db = 0.0, sum_dr2 = 0.0, sum_db2 = 0.0;
    uint64_t mid_count = 0;

    for (int gy = 0; gy < gh; ++gy) {
        const int y0 = gy * subsample;
        const int y1 = std::min(height, y0 + subsample);
        for (int gx = 0; gx < gw; ++gx) {
            const int x0 = gx * subsample;
            const int x1 = std::min(width, x0 + subsample);
            float sum_l = 0.0f;
            float sum_r_blk = 0.0f, sum_g_blk = 0.0f, sum_b_blk = 0.0f;
            int cnt = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = rgb + (static_cast<size_t>(y) * width + x0) * 3u;
                for (int x = x0; x < x1; ++x) {
                    const uint8_t* px = row + (x - x0) * 3;
                    const float r = static_cast<float>(px[0]);
                    const float g = static_cast<float>(px[1]);
                    const float b = static_cast<float>(px[2]);
                    sum_r_blk += r;
                    sum_g_blk += g;
                    sum_b_blk += b;
                    sum_l += 0.299f * r + 0.587f * g + 0.114f * b;
                    ++cnt;
                }
            }
            const float inv_c = 1.0f / static_cast<float>(std::max(1, cnt));
            const float avg_l255 = sum_l * inv_c;
            const float avg_l = avg_l255 * (1.0f / 255.0f);
            I_sub[static_cast<size_t>(gy) * gw + gx] = avg_l;
            II_sub[static_cast<size_t>(gy) * gw + gx] = avg_l * avg_l;

            const int bin = std::clamp(static_cast<int>(avg_l255 + 0.5f), 0, 255);
            hist_y[bin]++;

            if (avg_l255 > 28.0f && avg_l255 < 228.0f) {
                const double mr = static_cast<double>(sum_r_blk * inv_c);
                const double mg = static_cast<double>(sum_g_blk * inv_c);
                const double mb = static_cast<double>(sum_b_blk * inv_c);
                const double dr = mr - static_cast<double>(avg_l255);
                const double db = mb - static_cast<double>(avg_l255);
                sum_r += mr;
                sum_g += mg;
                sum_b += mb;
                sum_dr += dr;
                sum_db += db;
                sum_dr2 += dr * dr;
                sum_db2 += db * db;
                ++mid_count;
            }
        }
    }

    if (cancelled.load()) {
        return false;
    }

    // 2. Fast Self-Guided Filter (r_sub ~ 1/16 of image -> halo-free local base illumination)
    const int gf_radius = std::clamp(std::max(gw, gh) / 18, 4, 28);
    constexpr float kGuidedEps = 0.018f; // Preserves sharp skyline/hair step edges without halo

    box_blur_2d(I_sub.data(), tmp_grid.data(), mean_I.data(), gw, gh, gf_radius);
    box_blur_2d(II_sub.data(), tmp_grid.data(), corr_I.data(), gw, gh, gf_radius);

    for (size_t i = 0; i < gn; ++i) {
        const float m = mean_I[i];
        const float var = std::max(0.0f, corr_I[i] - m * m);
        const float a = var / (var + kGuidedEps);
        const float b = m - a * m;
        II_sub[i] = a;
        corr_I[i] = b;
    }

    box_blur_2d(II_sub.data(), tmp_grid.data(), a_grid.data(), gw, gh, gf_radius);
    box_blur_2d(corr_I.data(), tmp_grid.data(), b_grid.data(), gw, gh, gf_radius);

    if (cancelled.load()) {
        return false;
    }

    // 3. Adaptive HDR scene exposure targets
    double mean_y = 0.0;
    uint64_t dark_cnt = 0, bright_cnt = 0;
    for (int i = 0; i < 256; ++i) {
        mean_y += static_cast<double>(i) * static_cast<double>(hist_y[i]);
        if (i < 64) dark_cnt += hist_y[i];
        if (i > 205) bright_cnt += hist_y[i];
    }
    mean_y /= static_cast<double>(std::max<size_t>(1, gn)) * 255.0;
    const float dark_frac = static_cast<float>(dark_cnt) / static_cast<float>(std::max<size_t>(1, gn));
    const float bright_frac = static_cast<float>(bright_cnt) / static_cast<float>(std::max<size_t>(1, gn));

    // Automatically lift shadows stronger on backlit/underexposed photos, gentler on bright studio shots
    const float underexp_boost = std::clamp((0.52f - static_cast<float>(mean_y)) * 1.4f + dark_frac * 0.55f, 0.0f, 0.85f);
    const float shadow_lift = s * (0.28f + 0.44f * underexp_boost);
    const float highlight_comp = s * (0.18f + 0.30f * std::clamp(bright_frac * 1.6f, 0.0f, 0.65f));
    const float mid_contrast = s * 0.14f;

    bool is_color = false;
    float wb_r = 1.0f, wb_g = 1.0f, wb_b = 1.0f;
    if (mid_count > 16) {
        const double inv_n = 1.0 / static_cast<double>(mid_count);
        const double mdr = sum_dr * inv_n;
        const double mdb = sum_db * inv_n;
        const double chroma_std = std::sqrt(
            std::max(0.0, sum_dr2 * inv_n - mdr * mdr) +
            std::max(0.0, sum_db2 * inv_n - mdb * mdb)
        );
        is_color = (chroma_std > 6.5);
        if (is_color) {
            const float mr = static_cast<float>(sum_r * inv_n);
            const float mg = static_cast<float>(sum_g * inv_n);
            const float mb = static_cast<float>(sum_b * inv_n);
            const float m_avg = (mr + mg + mb) / 3.0f;
            if (mr > 10.0f && mg > 10.0f && mb > 10.0f) {
                // Subtle natural white balance (<= +-8%) so golden-hour mood is respected
                const float rr = std::clamp(m_avg / mr, 0.92f, 1.08f);
                const float rg = std::clamp(m_avg / mg, 0.94f, 1.06f);
                const float rb = std::clamp(m_avg / mb, 0.92f, 1.09f);
                const float wb_mix = 0.45f * s;
                wb_r = 1.0f + wb_mix * (rr - 1.0f);
                wb_g = 1.0f + wb_mix * (rg - 1.0f);
                wb_b = 1.0f + wb_mix * (rb - 1.0f);
            }
        }
    }
    const float vibrance = is_color ? (0.18f * s) : 0.0f;

    // 4. Full-resolution zero-halo relighting & natural vibrance pass in-place
    const float scale_x = static_cast<float>(gw - 1) / static_cast<float>(std::max(1, width - 1));
    const float scale_y = static_cast<float>(gh - 1) / static_cast<float>(std::max(1, height - 1));
    const size_t row_bytes = static_cast<size_t>(width) * 3u;

    #pragma omp parallel for schedule(static) if (height >= 32)
    for (int y = 0; y < height; ++y) {
        const float gy = static_cast<float>(y) * scale_y;
        uint8_t* row = rgb + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < width; ++x) {
            const float gx = static_cast<float>(x) * scale_x;
            uint8_t* px = row + static_cast<size_t>(x) * 3u;

            float r = (static_cast<float>(px[0]) * (1.0f / 255.0f)) * wb_r;
            float g = (static_cast<float>(px[1]) * (1.0f / 255.0f)) * wb_g;
            float b = (static_cast<float>(px[2]) * (1.0f / 255.0f)) * wb_b;

            const float lum = std::clamp(0.299f * r + 0.587f * g + 0.114f * b, 0.0f, 1.0f);
            const float a = sample_bilinear(a_grid.data(), gw, gh, gx, gy);
            const float bg = sample_bilinear(b_grid.data(), gw, gh, gx, gy);
            const float base = std::clamp(a * lum + bg, 1e-3f, 1.0f);

            // Map base illumination: photographic sqrt-toe shadow lift + highlight roll-off + midtone S-curve
            const float inv_base = 1.0f - base;
            float mapped_base = base + shadow_lift * std::sqrt(base) * inv_base * inv_base;
            if (mapped_base > 0.62f) {
                const float hi = (mapped_base - 0.62f) / 0.38f;
                mapped_base -= highlight_comp * 0.22f * hi * hi;
            }
            const float scurve = mapped_base * mapped_base * (3.0f - 2.0f * mapped_base);
            mapped_base = std::clamp(mapped_base + mid_contrast * (scurve - mapped_base), 0.0f, 1.0f);

            // Preserve crisp local detail ratio without amplifying deep-black shadow noise
            const float detail_ratio = (lum + 0.025f) / (base + 0.025f);
            const float lum_out = std::clamp(mapped_base * detail_ratio, 0.0f, 1.0f);

            // Luminance-preserving color scaling (prevents shadow-lift color washout)
            const float gain = (lum > 1e-3f) ? (lum_out / lum) : 1.0f;
            const float color_gamma = (gain > 1.0f) ? std::pow(gain, 0.88f) : gain;
            float nr = lum_out + (r - lum) * color_gamma;
            float ng = lum_out + (g - lum) * color_gamma;
            float nb = lum_out + (b - lum) * color_gamma;

            if (vibrance > 0.0f) {
                const float mx = std::max({nr, ng, nb});
                const float mn = std::min({nr, ng, nb});
                const float sat = (mx > 1e-3f) ? std::clamp((mx - mn) / mx, 0.0f, 1.0f) : 0.0f;
                // Protect skin tones (warm R > G > B hues) from over-saturation
                const bool skin_like = (nr > ng && ng > nb && (nr - nb) > 0.04f && sat < 0.65f);
                const float vib_gain = 1.0f + vibrance * (1.0f - sat) * (skin_like ? 0.38f : 1.0f);
                nr = lum_out + (nr - lum_out) * vib_gain;
                ng = lum_out + (ng - lum_out) * vib_gain;
                nb = lum_out + (nb - lum_out) * vib_gain;
            }

            px[0] = clamp_u8(nr * 255.0f);
            px[1] = clamp_u8(ng * 255.0f);
            px[2] = clamp_u8(nb * 255.0f);
        }
    }

    return true;
}

} // namespace enhancer
