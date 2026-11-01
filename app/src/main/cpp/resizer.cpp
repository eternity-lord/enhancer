#include "resizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace enhancer {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

inline double sinc(double x) noexcept {
    if (std::abs(x) < 1e-8) {
        return 1.0;
    }
    const double pix = kPi * x;
    return std::sin(pix) / pix;
}

inline double lanczos3_kernel(double x) noexcept {
    const double ax = std::abs(x);
    if (ax >= 3.0) {
        return 0.0;
    }
    return sinc(x) * sinc(x / 3.0);
}

inline uint8_t clamp_float_to_u8(float v) noexcept {
    const float rounded = v + 0.5f;
    if (!(rounded > 0.0f)) {
        return 0;
    }
    if (rounded >= 255.0f) {
        return 255;
    }
    return static_cast<uint8_t>(rounded);
}

struct BoxSpan {
    int start = 0;
    int count = 0;
    int weight_offset = 0;
};

struct LanczosTap6 {
    int idx[6];
    float w[6];
};

std::vector<LanczosTap6> build_lanczos3_taps(int src_len, int dst_len) {
    std::vector<LanczosTap6> table(static_cast<size_t>(dst_len));
    const double scale = static_cast<double>(src_len) / static_cast<double>(dst_len);

    for (int d = 0; d < dst_len; ++d) {
        const double center = (static_cast<double>(d) + 0.5) * scale - 0.5;
        const int base = static_cast<int>(std::floor(center)) - 2;

        double sum = 0.0;
        double raw_w[6];
        for (int k = 0; k < 6; ++k) {
            const int s = base + k;
            table[d].idx[k] = std::clamp(s, 0, src_len - 1);
            const double w = lanczos3_kernel(center - static_cast<double>(s));
            raw_w[k] = w;
            sum += w;
        }

        const double inv_sum = (std::abs(sum) > 1e-12) ? (1.0 / sum) : 0.0;
        for (int k = 0; k < 6; ++k) {
            table[d].w[k] = static_cast<float>(raw_w[k] * inv_sum);
        }
    }
    return table;
}

} // namespace

DimensionPlan compute_dimensions(int src_w, int src_h, int model_scale, int target_long_side) {
    DimensionPlan plan;
    plan.src_w = src_w;
    plan.src_h = src_h;

    if (src_w <= 0 || src_h <= 0 || model_scale <= 0) {
        return plan;
    }

    if (target_long_side <= 0) {
        // Native xN mode: run model directly with no pre/post resize
        plan.work_w = src_w;
        plan.work_h = src_h;
        plan.model_out_w = src_w * model_scale;
        plan.model_out_h = src_h * model_scale;
        plan.final_w = plan.model_out_w;
        plan.final_h = plan.model_out_h;
        plan.pre_downscaled = false;
        plan.post_upscaled = false;
        return plan;
    }

    const int src_long = std::max(src_w, src_h);
    const int64_t native_upscaled_long = static_cast<int64_t>(src_long) * static_cast<int64_t>(model_scale);

    if (native_upscaled_long >= static_cast<int64_t>(target_long_side)) {
        // Rule 2: source_long * m >= target
        // Downscale source to (target / m) long side first, then run model once.
        const int work_long = std::max(1, target_long_side / model_scale);
        if (src_w >= src_h) {
            plan.work_w = work_long;
            plan.work_h = std::max(
                1,
                static_cast<int>(std::lround(static_cast<double>(src_h) * work_long / static_cast<double>(src_w)))
            );
        } else {
            plan.work_h = work_long;
            plan.work_w = std::max(
                1,
                static_cast<int>(std::lround(static_cast<double>(src_w) * work_long / static_cast<double>(src_h)))
            );
        }
        plan.model_out_w = plan.work_w * model_scale;
        plan.model_out_h = plan.work_h * model_scale;
        plan.final_w = plan.model_out_w;
        plan.final_h = plan.model_out_h;
        plan.pre_downscaled = (plan.work_w < src_w || plan.work_h < src_h);
        plan.post_upscaled = false;
    } else {
        // Rule 3: source_long * m < target
        // Run model on source at native size, then Lanczos3 upscale to exact target.
        plan.work_w = src_w;
        plan.work_h = src_h;
        plan.model_out_w = src_w * model_scale;
        plan.model_out_h = src_h * model_scale;
        if (src_w >= src_h) {
            plan.final_w = target_long_side;
            plan.final_h = std::max(
                1,
                static_cast<int>(std::lround(static_cast<double>(src_h) * target_long_side / static_cast<double>(src_w)))
            );
        } else {
            plan.final_h = target_long_side;
            plan.final_w = std::max(
                1,
                static_cast<int>(std::lround(static_cast<double>(src_w) * target_long_side / static_cast<double>(src_h)))
            );
        }
        plan.pre_downscaled = false;
        plan.post_upscaled = true;
    }

    return plan;
}

bool downscale_rgba_to_rgb_box(
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag
) {
    if (!src_rgba || !dst_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }
    if (src_stride_bytes < src_w * 4) {
        return false;
    }

    // Fast path for exact 1:1 copy
    if (src_w == dst_w && src_h == dst_h) {
        for (int y = 0; y < dst_h; ++y) {
            if ((y & 63) == 0 && cancel_flag.load(std::memory_order_relaxed)) {
                return false;
            }
            const uint8_t* srow = src_rgba + static_cast<size_t>(y) * static_cast<size_t>(src_stride_bytes);
            uint8_t* drow = dst_rgb + static_cast<size_t>(y) * static_cast<size_t>(dst_w) * 3;
            for (int x = 0; x < dst_w; ++x) {
                drow[x * 3 + 0] = srow[x * 4 + 0];
                drow[x * 3 + 1] = srow[x * 4 + 1];
                drow[x * 3 + 2] = srow[x * 4 + 2];
            }
        }
        return true;
    }

    const double scale_x = static_cast<double>(src_w) / static_cast<double>(dst_w);
    const double scale_y = static_cast<double>(src_h) / static_cast<double>(dst_h);

    // Precompute horizontal box filter spans & weights
    std::vector<BoxSpan> x_spans(static_cast<size_t>(dst_w));
    std::vector<float> x_weights;
    x_weights.reserve(static_cast<size_t>(src_w + dst_w * 2));

    for (int dx = 0; dx < dst_w; ++dx) {
        const double x0 = dx * scale_x;
        const double x1 = std::min(static_cast<double>(src_w), (dx + 1) * scale_x);
        const int sx0 = std::clamp(static_cast<int>(std::floor(x0)), 0, src_w - 1);
        const int sx1 = std::clamp(static_cast<int>(std::ceil(x1)) - 1, sx0, src_w - 1);

        BoxSpan span;
        span.start = sx0;
        span.count = sx1 - sx0 + 1;
        span.weight_offset = static_cast<int>(x_weights.size());

        double w_sum = 0.0;
        for (int sx = sx0; sx <= sx1; ++sx) {
            const double overlap = std::min(x1, static_cast<double>(sx + 1)) - std::max(x0, static_cast<double>(sx));
            const float w = static_cast<float>(std::max(0.0, overlap));
            x_weights.push_back(w);
            w_sum += w;
        }
        if (w_sum > 0.0) {
            const float inv = static_cast<float>(1.0 / w_sum);
            for (int i = 0; i < span.count; ++i) {
                x_weights[static_cast<size_t>(span.weight_offset + i)] *= inv;
            }
        } else {
            x_weights[static_cast<size_t>(span.weight_offset)] = 1.0f;
        }
        x_spans[static_cast<size_t>(dx)] = span;
    }

    // Precompute vertical box filter spans & weights
    std::vector<BoxSpan> y_spans(static_cast<size_t>(dst_h));
    std::vector<float> y_weights;
    y_weights.reserve(static_cast<size_t>(src_h + dst_h * 2));

    for (int dy = 0; dy < dst_h; ++dy) {
        const double y0 = dy * scale_y;
        const double y1 = std::min(static_cast<double>(src_h), (dy + 1) * scale_y);
        const int sy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, src_h - 1);
        const int sy1 = std::clamp(static_cast<int>(std::ceil(y1)) - 1, sy0, src_h - 1);

        BoxSpan span;
        span.start = sy0;
        span.count = sy1 - sy0 + 1;
        span.weight_offset = static_cast<int>(y_weights.size());

        double w_sum = 0.0;
        for (int sy = sy0; sy <= sy1; ++sy) {
            const double overlap = std::min(y1, static_cast<double>(sy + 1)) - std::max(y0, static_cast<double>(sy));
            const float w = static_cast<float>(std::max(0.0, overlap));
            y_weights.push_back(w);
            w_sum += w;
        }
        if (w_sum > 0.0) {
            const float inv = static_cast<float>(1.0 / w_sum);
            for (int i = 0; i < span.count; ++i) {
                y_weights[static_cast<size_t>(span.weight_offset + i)] *= inv;
            }
        } else {
            y_weights[static_cast<size_t>(span.weight_offset)] = 1.0f;
        }
        y_spans[static_cast<size_t>(dy)] = span;
    }

    #pragma omp parallel for schedule(static)
    for (int dy = 0; dy < dst_h; ++dy) {
        if (cancel_flag.load(std::memory_order_relaxed)) {
            continue;
        }
        const BoxSpan& y_span = y_spans[static_cast<size_t>(dy)];
        const float* wy_ptr = y_weights.data() + y_span.weight_offset;
        uint8_t* dst_row = dst_rgb + static_cast<size_t>(dy) * static_cast<size_t>(dst_w) * 3;

        for (int dx = 0; dx < dst_w; ++dx) {
            const BoxSpan& x_span = x_spans[static_cast<size_t>(dx)];
            const float* wx_ptr = x_weights.data() + x_span.weight_offset;

            float acc_r = 0.0f;
            float acc_g = 0.0f;
            float acc_b = 0.0f;

            for (int iy = 0; iy < y_span.count; ++iy) {
                const float wy = wy_ptr[iy];
                const uint8_t* src_row = src_rgba +
                    static_cast<size_t>(y_span.start + iy) * static_cast<size_t>(src_stride_bytes) +
                    static_cast<size_t>(x_span.start) * 4;

                float row_r = 0.0f;
                float row_g = 0.0f;
                float row_b = 0.0f;
                for (int ix = 0; ix < x_span.count; ++ix) {
                    const float wx = wx_ptr[ix];
                    const uint8_t* px = src_row + ix * 4;
                    row_r += static_cast<float>(px[0]) * wx;
                    row_g += static_cast<float>(px[1]) * wx;
                    row_b += static_cast<float>(px[2]) * wx;
                }
                acc_r += row_r * wy;
                acc_g += row_g * wy;
                acc_b += row_b * wy;
            }

            dst_row[dx * 3 + 0] = clamp_float_to_u8(acc_r);
            dst_row[dx * 3 + 1] = clamp_float_to_u8(acc_g);
            dst_row[dx * 3 + 2] = clamp_float_to_u8(acc_b);
        }
    }

    return !cancel_flag.load(std::memory_order_relaxed);
}

bool upscale_rgb_lanczos3_streaming(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pct_0_to_100)>& progress_cb
) {
    if (!src_rgb || !dst_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    if (src_w == dst_w && src_h == dst_h) {
        std::memcpy(dst_rgb, src_rgb, static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3);
        if (progress_cb) {
            progress_cb(100);
        }
        return true;
    }

    const std::vector<LanczosTap6> x_taps = build_lanczos3_taps(src_w, dst_w);
    const std::vector<LanczosTap6> y_taps = build_lanczos3_taps(src_h, dst_h);

    // 8-slot rolling horizontal row cache:
    // Because vertical Lanczos3 uses at most 6 monotonically advancing source rows (sy0..sy0+5),
    // modulo-8 indexing never collides within any single output row's 6-tap window.
    constexpr int kCacheSlots = 8;
    const size_t row_floats = static_cast<size_t>(dst_w) * 3;
    std::vector<float> row_cache(static_cast<size_t>(kCacheSlots) * row_floats, 0.0f);
    int cached_sy[kCacheSlots];
    for (int i = 0; i < kCacheSlots; ++i) {
        cached_sy[i] = -1;
    }

    auto ensure_horizontal_row = [&](int sy) -> const float* {
        const int slot = sy & (kCacheSlots - 1);
        float* slot_ptr = row_cache.data() + static_cast<size_t>(slot) * row_floats;
        if (cached_sy[slot] == sy) {
            return slot_ptr;
        }

        const uint8_t* srow = src_rgb + static_cast<size_t>(sy) * static_cast<size_t>(src_w) * 3;

        #pragma omp parallel for schedule(static)
        for (int dx = 0; dx < dst_w; ++dx) {
            const LanczosTap6& tap = x_taps[static_cast<size_t>(dx)];
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            for (int k = 0; k < 6; ++k) {
                const uint8_t* spx = srow + static_cast<size_t>(tap.idx[k]) * 3;
                const float w = tap.w[k];
                r += static_cast<float>(spx[0]) * w;
                g += static_cast<float>(spx[1]) * w;
                b += static_cast<float>(spx[2]) * w;
            }
            slot_ptr[static_cast<size_t>(dx) * 3 + 0] = r;
            slot_ptr[static_cast<size_t>(dx) * 3 + 1] = g;
            slot_ptr[static_cast<size_t>(dx) * 3 + 2] = b;
        }

        cached_sy[slot] = sy;
        return slot_ptr;
    };

    int last_reported_pct = -1;
    for (int dy = 0; dy < dst_h; ++dy) {
        if ((dy & 31) == 0 && cancel_flag.load(std::memory_order_relaxed)) {
            return false;
        }

        const LanczosTap6& ytap = y_taps[static_cast<size_t>(dy)];
        const float* hrows[6];
        for (int k = 0; k < 6; ++k) {
            hrows[k] = ensure_horizontal_row(ytap.idx[k]);
        }

        const float wy0 = ytap.w[0];
        const float wy1 = ytap.w[1];
        const float wy2 = ytap.w[2];
        const float wy3 = ytap.w[3];
        const float wy4 = ytap.w[4];
        const float wy5 = ytap.w[5];

        uint8_t* out_row = dst_rgb + static_cast<size_t>(dy) * row_floats;

        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < row_floats; ++i) {
            const float val =
                hrows[0][i] * wy0 +
                hrows[1][i] * wy1 +
                hrows[2][i] * wy2 +
                hrows[3][i] * wy3 +
                hrows[4][i] * wy4 +
                hrows[5][i] * wy5;
            out_row[i] = clamp_float_to_u8(val);
        }

        if (progress_cb && ((dy & 63) == 0 || dy + 1 == dst_h)) {
            const int pct = static_cast<int>((static_cast<int64_t>(dy + 1) * 100) / dst_h);
            if (pct != last_reported_pct) {
                last_reported_pct = pct;
                progress_cb(pct);
            }
        }
    }

    return true;
}

} // namespace enhancer
