#include "resizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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

// Catmull-Rom bicubic spline (Keys a = -0.5)
inline float catmull_rom_weight(float x) noexcept {
    const float ax = std::abs(x);
    if (ax < 1.0f) {
        return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
    }
    if (ax < 2.0f) {
        return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
    }
    return 0.0f;
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

} // namespace

DimensionPlan compute_dimensions(
    int src_w,
    int src_h,
    int model_scale,
    int target_long_side,
    bool max_quality
) {
    DimensionPlan plan;
    plan.src_w = src_w;
    plan.src_h = src_h;

    if (src_w <= 0 || src_h <= 0 || model_scale <= 0) {
        return plan;
    }

    const int64_t scaled_src_w = static_cast<int64_t>(src_w) * static_cast<int64_t>(model_scale);
    const int64_t scaled_src_h = static_cast<int64_t>(src_h) * static_cast<int64_t>(model_scale);
    if (scaled_src_w > static_cast<int64_t>(std::numeric_limits<int>::max() / 4) ||
        scaled_src_h > static_cast<int64_t>(std::numeric_limits<int>::max() / 4)) {
        return DimensionPlan{};
    }

    if (target_long_side <= 0) {
        // Native xN mode: run model directly with no pre/post resize
        plan.work_w = src_w;
        plan.work_h = src_h;
        plan.model_out_w = static_cast<int>(scaled_src_w);
        plan.model_out_h = static_cast<int>(scaled_src_h);
        plan.final_w = plan.model_out_w;
        plan.final_h = plan.model_out_h;
        return plan;
    }

    if (max_quality) {
        // Max quality mode: ALWAYS run the model on the full-resolution source (never pre-downscale).
        plan.work_w = src_w;
        plan.work_h = src_h;
        plan.model_out_w = static_cast<int>(scaled_src_w);
        plan.model_out_h = static_cast<int>(scaled_src_h);

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
        const int model_long = std::max(plan.model_out_w, plan.model_out_h);
        if (model_long > target_long_side) {
            plan.post_downscaled = true;
            plan.post_upscaled = false;
        } else if (model_long < target_long_side) {
            plan.post_downscaled = false;
            plan.post_upscaled = true;
        }
        return plan;
    }

    const int src_long = std::max(src_w, src_h);
    const int64_t native_upscaled_long =
        static_cast<int64_t>(src_long) * static_cast<int64_t>(model_scale);

    if (native_upscaled_long >= static_cast<int64_t>(target_long_side)) {
        // Standard Rule 2: source_long * m >= target -> area/box pre-downscale to target / m
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
        plan.model_out_w = static_cast<int>(static_cast<int64_t>(plan.work_w) * static_cast<int64_t>(model_scale));
        plan.model_out_h = static_cast<int>(static_cast<int64_t>(plan.work_h) * static_cast<int64_t>(model_scale));
        plan.final_w = plan.model_out_w;
        plan.final_h = plan.model_out_h;
        plan.pre_downscaled = (plan.work_w < src_w || plan.work_h < src_h);
        plan.post_upscaled = false;
        plan.post_downscaled = false;
    } else {
        // Standard Rule 3: source_long * m < target -> native AI pass + Lanczos3 upscale
        plan.work_w = src_w;
        plan.work_h = src_h;
        plan.model_out_w = static_cast<int>(scaled_src_w);
        plan.model_out_h = static_cast<int>(scaled_src_h);
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
        plan.post_downscaled = false;
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
    if (static_cast<int64_t>(src_stride_bytes) < static_cast<int64_t>(src_w) * 4) {
        return false;
    }

    if (src_w == dst_w && src_h == dst_h) {
        for (int y = 0; y < dst_h; ++y) {
            if ((y & 63) == 0 && cancel_flag.load(std::memory_order_relaxed)) {
                return false;
            }
            const uint8_t* srow = src_rgba + static_cast<size_t>(y) * static_cast<size_t>(src_stride_bytes);
            uint8_t* drow = dst_rgb + static_cast<size_t>(y) * static_cast<size_t>(dst_w) * 3;
            for (int x = 0; x < dst_w; ++x) {
                const size_t dx3 = static_cast<size_t>(x) * 3;
                const size_t sx4 = static_cast<size_t>(x) * 4;
                drow[dx3 + 0] = srow[sx4 + 0];
                drow[dx3 + 1] = srow[sx4 + 1];
                drow[dx3 + 2] = srow[sx4 + 2];
            }
        }
        return true;
    }

    const double scale_x = static_cast<double>(src_w) / static_cast<double>(dst_w);
    const double scale_y = static_cast<double>(src_h) / static_cast<double>(dst_h);

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
                    const uint8_t* px = src_row + static_cast<size_t>(ix) * 4;
                    row_r += static_cast<float>(px[0]) * wx;
                    row_g += static_cast<float>(px[1]) * wx;
                    row_b += static_cast<float>(px[2]) * wx;
                }
                acc_r += row_r * wy;
                acc_g += row_g * wy;
                acc_b += row_b * wy;
            }

            const size_t dx3 = static_cast<size_t>(dx) * 3;
            dst_row[dx3 + 0] = clamp_float_to_u8(acc_r);
            dst_row[dx3 + 1] = clamp_float_to_u8(acc_g);
            dst_row[dx3 + 2] = clamp_float_to_u8(acc_b);
        }
    }

    return !cancel_flag.load(std::memory_order_relaxed);
}

bool StreamingLanczos3Resizer::init(
    int src_w,
    int src_h,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    std::function<void(int pct_0_to_100)> progress_cb
) {
    if (!dst_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    src_w_ = src_w;
    src_h_ = src_h;
    dst_rgb_ = dst_rgb;
    dst_w_ = dst_w;
    dst_h_ = dst_h;
    progress_cb_ = std::move(progress_cb);

    auto build_axis_table = [](
        int src_len,
        int dst_len,
        std::vector<VarTapSpan>& out_spans,
        std::vector<float>& out_weights,
        int& out_max_taps
    ) {
        out_spans.resize(static_cast<size_t>(dst_len));
        out_weights.clear();

        const double scale = static_cast<double>(src_len) / static_cast<double>(dst_len);
        // For downscaling (scale > 1), widen kernel support by `scale` for proper anti-aliasing.
        const double filter_scale = std::max(1.0, scale);
        const double support = 3.0 * filter_scale;

        out_weights.reserve(static_cast<size_t>(dst_len) * static_cast<size_t>(std::ceil(support * 2.0) + 2));
        out_max_taps = 1;

        std::vector<double> folded;
        for (int d = 0; d < dst_len; ++d) {
            const double center = (static_cast<double>(d) + 0.5) * scale - 0.5;
            const int raw_lo = static_cast<int>(std::ceil(center - support));
            const int raw_hi = static_cast<int>(std::floor(center + support));

            const int clamped_lo = std::clamp(raw_lo, 0, src_len - 1);
            const int clamped_hi = std::clamp(raw_hi, clamped_lo, src_len - 1);
            const int span_count = clamped_hi - clamped_lo + 1;
            out_max_taps = std::max(out_max_taps, span_count);

            folded.assign(static_cast<size_t>(span_count), 0.0);
            double sum = 0.0;
            for (int s = raw_lo; s <= raw_hi; ++s) {
                const double w = lanczos3_kernel((center - static_cast<double>(s)) / filter_scale);
                const int cs = std::clamp(s, 0, src_len - 1);
                folded[static_cast<size_t>(cs - clamped_lo)] += w;
                sum += w;
            }

            VarTapSpan span;
            span.start_idx = clamped_lo;
            span.count = span_count;
            span.weight_offset = static_cast<int>(out_weights.size());

            const double inv = (std::abs(sum) > 1e-12) ? (1.0 / sum) : 0.0;
            for (int i = 0; i < span_count; ++i) {
                out_weights.push_back(static_cast<float>(folded[static_cast<size_t>(i)] * inv));
            }
            out_spans[static_cast<size_t>(d)] = span;
        }
    };

    int max_x_taps = 0;
    int max_y_taps = 0;
    build_axis_table(src_w_, dst_w_, x_spans_, x_weights_, max_x_taps);
    build_axis_table(src_h_, dst_h_, y_spans_, y_weights_, max_y_taps);

    // Next power-of-two >= max_y_taps + 2 so modulo masking never collides within any output row's window
    ring_slots_ = 8;
    while (ring_slots_ < max_y_taps + 2) {
        ring_slots_ *= 2;
    }

    size_t ring_elems = 0;
    if (!checked_mul_size(static_cast<size_t>(dst_w_), 3u, row_floats_) ||
        !checked_mul_size(static_cast<size_t>(ring_slots_), row_floats_, ring_elems)) {
        return false;
    }
    row_ring_.assign(ring_elems, 0.0f);
    slot_sy_.assign(static_cast<size_t>(ring_slots_), -1);

    reset_stream();
    return true;
}

void StreamingLanczos3Resizer::reset_stream() {
    pushed_src_rows_ = 0;
    next_dst_y_ = 0;
    last_reported_pct_ = -1;
    std::fill(slot_sy_.begin(), slot_sy_.end(), -1);
}

void StreamingLanczos3Resizer::compute_horizontal_row(const uint8_t* src_row, int sy) {
    const int slot = sy & (ring_slots_ - 1);
    float* slot_ptr = row_ring_.data() + static_cast<size_t>(slot) * row_floats_;

    #pragma omp parallel for schedule(static)
    for (int dx = 0; dx < dst_w_; ++dx) {
        const VarTapSpan& span = x_spans_[static_cast<size_t>(dx)];
        const float* w_ptr = x_weights_.data() + span.weight_offset;
        const uint8_t* spx_base = src_row + static_cast<size_t>(span.start_idx) * 3;

        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        for (int k = 0; k < span.count; ++k) {
            const float w = w_ptr[k];
            const uint8_t* spx = spx_base + static_cast<size_t>(k) * 3;
            r += static_cast<float>(spx[0]) * w;
            g += static_cast<float>(spx[1]) * w;
            b += static_cast<float>(spx[2]) * w;
        }
        slot_ptr[static_cast<size_t>(dx) * 3 + 0] = r;
        slot_ptr[static_cast<size_t>(dx) * 3 + 1] = g;
        slot_ptr[static_cast<size_t>(dx) * 3 + 2] = b;
    }

    slot_sy_[static_cast<size_t>(slot)] = sy;
}

bool StreamingLanczos3Resizer::push_source_rows(
    const uint8_t* src_rows_rgb,
    int row_count,
    const std::atomic<bool>& cancel_flag
) {
    if (!src_rows_rgb || row_count <= 0) {
        return row_count == 0;
    }

    // Fast 1:1 identity copy path
    if (src_w_ == dst_w_ && src_h_ == dst_h_) {
        const int copy_rows = std::min(row_count, dst_h_ - next_dst_y_);
        if (copy_rows > 0) {
            std::memcpy(
                dst_rgb_ + static_cast<size_t>(next_dst_y_) * row_floats_,
                src_rows_rgb,
                static_cast<size_t>(copy_rows) * row_floats_
            );
            next_dst_y_ += copy_rows;
            pushed_src_rows_ += copy_rows;
            if (progress_cb_) {
                const int pct = static_cast<int>((static_cast<int64_t>(next_dst_y_) * 100) / dst_h_);
                if (pct != last_reported_pct_) {
                    last_reported_pct_ = pct;
                    progress_cb_(pct);
                }
            }
        }
        return !cancel_flag.load(std::memory_order_relaxed);
    }

    const size_t src_row_bytes = static_cast<size_t>(src_w_) * 3;
    std::vector<const float*> active_hrows;

    for (int r = 0; r < row_count && pushed_src_rows_ < src_h_; ++r) {
        if ((r & 15) == 0 && cancel_flag.load(std::memory_order_relaxed)) {
            return false;
        }

        const int sy = pushed_src_rows_;
        // Only compute horizontal resample for `sy` if at least one remaining output row actually uses it
        bool needed = false;
        if (next_dst_y_ < dst_h_) {
            const VarTapSpan& cur_y = y_spans_[static_cast<size_t>(next_dst_y_)];
            if (sy >= cur_y.start_idx) {
                needed = true;
            }
        }
        if (needed) {
            compute_horizontal_row(src_rows_rgb + static_cast<size_t>(r) * src_row_bytes, sy);
        }
        ++pushed_src_rows_;

        // Emit all output rows `dy` whose vertical tap window is now fully available
        while (next_dst_y_ < dst_h_) {
            const VarTapSpan& yspan = y_spans_[static_cast<size_t>(next_dst_y_)];
            const int max_needed_sy = yspan.start_idx + yspan.count - 1;
            if (max_needed_sy >= pushed_src_rows_) {
                break;
            }

            if ((next_dst_y_ & 31) == 0 && cancel_flag.load(std::memory_order_relaxed)) {
                return false;
            }

            active_hrows.resize(static_cast<size_t>(yspan.count));
            for (int k = 0; k < yspan.count; ++k) {
                const int needed_sy = yspan.start_idx + k;
                const int slot = needed_sy & (ring_slots_ - 1);
                active_hrows[static_cast<size_t>(k)] =
                    row_ring_.data() + static_cast<size_t>(slot) * row_floats_;
            }

            const float* wy_ptr = y_weights_.data() + yspan.weight_offset;
            const int tap_count = yspan.count;
            const float* const* hrows_ptr = active_hrows.data();
            uint8_t* out_row = dst_rgb_ + static_cast<size_t>(next_dst_y_) * row_floats_;

            const ptrdiff_t rf_count = static_cast<ptrdiff_t>(row_floats_);
            #pragma omp parallel for schedule(static)
            for (ptrdiff_t i = 0; i < rf_count; ++i) {
                float acc = 0.0f;
                for (int k = 0; k < tap_count; ++k) {
                    acc += hrows_ptr[k][i] * wy_ptr[k];
                }
                out_row[i] = clamp_float_to_u8(acc);
            }

            ++next_dst_y_;

            if (progress_cb_ && ((next_dst_y_ & 63) == 0 || next_dst_y_ == dst_h_)) {
                const int pct = static_cast<int>((static_cast<int64_t>(next_dst_y_) * 100) / dst_h_);
                if (pct != last_reported_pct_) {
                    last_reported_pct_ = pct;
                    progress_cb_(pct);
                }
            }
        }
    }

    return !cancel_flag.load(std::memory_order_relaxed);
}

bool resize_rgb_lanczos3_streaming(
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
    StreamingLanczos3Resizer resizer;
    if (!resizer.init(src_w, src_h, dst_rgb, dst_w, dst_h, progress_cb)) {
        return false;
    }
    return resizer.push_source_rows(src_rgb, src_h, cancel_flag) &&
           resizer.next_dst_row() == dst_h;
}

bool resize_rgba_roi_bicubic(
    const uint8_t* src_rgba,
    int src_w,
    int src_h,
    int src_stride_bytes,
    double roi_x0,
    double roi_y0,
    double roi_x1,
    double roi_y1,
    uint8_t* dst_rgba,
    int dst_w,
    int dst_h,
    int dst_stride_bytes
) {
    if (!src_rgba || !dst_rgba ||
        src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        static_cast<int64_t>(src_stride_bytes) < static_cast<int64_t>(src_w) * 4 ||
        static_cast<int64_t>(dst_stride_bytes) < static_cast<int64_t>(dst_w) * 4) {
        return false;
    }

    const double roi_w = roi_x1 - roi_x0;
    const double roi_h = roi_y1 - roi_y0;
    if (roi_w <= 0.0 || roi_h <= 0.0) {
        return false;
    }

    struct CubicTap4 {
        int idx[4];
        float w[4];
    };

    std::vector<CubicTap4> x_taps(static_cast<size_t>(dst_w));
    for (int dx = 0; dx < dst_w; ++dx) {
        const double sx = roi_x0 + (static_cast<double>(dx) + 0.5) * (roi_w / static_cast<double>(dst_w)) - 0.5;
        const int base = static_cast<int>(std::floor(sx)) - 1;
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int idx = std::clamp(base + k, 0, src_w - 1);
            const float w = catmull_rom_weight(static_cast<float>(sx - static_cast<double>(base + k)));
            x_taps[static_cast<size_t>(dx)].idx[k] = idx;
            x_taps[static_cast<size_t>(dx)].w[k] = w;
            sum += w;
        }
        const float inv = (std::abs(sum) > 1e-6f) ? (1.0f / sum) : 1.0f;
        for (int k = 0; k < 4; ++k) {
            x_taps[static_cast<size_t>(dx)].w[k] *= inv;
        }
    }

    std::vector<CubicTap4> y_taps(static_cast<size_t>(dst_h));
    for (int dy = 0; dy < dst_h; ++dy) {
        const double sy = roi_y0 + (static_cast<double>(dy) + 0.5) * (roi_h / static_cast<double>(dst_h)) - 0.5;
        const int base = static_cast<int>(std::floor(sy)) - 1;
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int idx = std::clamp(base + k, 0, src_h - 1);
            const float w = catmull_rom_weight(static_cast<float>(sy - static_cast<double>(base + k)));
            y_taps[static_cast<size_t>(dy)].idx[k] = idx;
            y_taps[static_cast<size_t>(dy)].w[k] = w;
            sum += w;
        }
        const float inv = (std::abs(sum) > 1e-6f) ? (1.0f / sum) : 1.0f;
        for (int k = 0; k < 4; ++k) {
            y_taps[static_cast<size_t>(dy)].w[k] *= inv;
        }
    }

    #pragma omp parallel for schedule(static)
    for (int dy = 0; dy < dst_h; ++dy) {
        const CubicTap4& ytap = y_taps[static_cast<size_t>(dy)];
        const uint8_t* srows[4] = {
            src_rgba + static_cast<size_t>(ytap.idx[0]) * static_cast<size_t>(src_stride_bytes),
            src_rgba + static_cast<size_t>(ytap.idx[1]) * static_cast<size_t>(src_stride_bytes),
            src_rgba + static_cast<size_t>(ytap.idx[2]) * static_cast<size_t>(src_stride_bytes),
            src_rgba + static_cast<size_t>(ytap.idx[3]) * static_cast<size_t>(src_stride_bytes),
        };
        uint8_t* drow = dst_rgba + static_cast<size_t>(dy) * static_cast<size_t>(dst_stride_bytes);

        for (int dx = 0; dx < dst_w; ++dx) {
            const CubicTap4& xtap = x_taps[static_cast<size_t>(dx)];
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            for (int ky = 0; ky < 4; ++ky) {
                const float wy = ytap.w[ky];
                const uint8_t* sr = srows[ky];
                float rr = 0.0f;
                float gg = 0.0f;
                float bb = 0.0f;
                for (int kx = 0; kx < 4; ++kx) {
                    const float wx = xtap.w[kx];
                    const uint8_t* px = sr + static_cast<size_t>(xtap.idx[kx]) * 4;
                    rr += static_cast<float>(px[0]) * wx;
                    gg += static_cast<float>(px[1]) * wx;
                    bb += static_cast<float>(px[2]) * wx;
                }
                r += rr * wy;
                g += gg * wy;
                b += bb * wy;
            }
            const size_t dx4 = static_cast<size_t>(dx) * 4;
            drow[dx4 + 0] = clamp_float_to_u8(r);
            drow[dx4 + 1] = clamp_float_to_u8(g);
            drow[dx4 + 2] = clamp_float_to_u8(b);
            drow[dx4 + 3] = 255;
        }
    }

    return true;
}

} // namespace enhancer
