#ifndef ENHANCER_RESIZER_H
#define ENHANCER_RESIZER_H

#include "enhancer_types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace enhancer {

// Computes exact intermediate and final dimensions while preserving aspect ratio.
// When `max_quality` is true, never pre-downscales source before inference; if model output
// exceeds target, `post_downscaled` is set so streaming Lanczos3 supersampling downscales the output.
DimensionPlan compute_dimensions(
    int src_w,
    int src_h,
    int model_scale,
    int target_long_side,
    bool max_quality = false
);

// Exact pixel-coverage area/box filter downscaling RGBA_8888 (alpha ignored) to packed RGB888.
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

// Incremental streaming row-by-row separable Lanczos3 resizer supporting BOTH upscaling
// and anti-aliased supersampling downscaling with only a small rolling horizontal float ring buffer.
class StreamingLanczos3Resizer {
public:
    StreamingLanczos3Resizer() = default;

    bool init(
        int src_w,
        int src_h,
        uint8_t* dst_rgb,
        int dst_w,
        int dst_h,
        std::function<void(int pct_0_to_100)> progress_cb = nullptr
    );

    // Resets streaming state so the resizer can restart from row 0 if tile size halves mid-run.
    void reset_stream();

    // Pushes `row_count` consecutive packed RGB888 rows of width `src_w_` starting at `pushed_src_rows_`.
    // Immediately computes and writes all output rows whose vertical tap window is satisfied.
    bool push_source_rows(
        const uint8_t* src_rows_rgb,
        int row_count,
        const std::atomic<bool>& cancel_flag
    );

    int next_dst_row() const noexcept { return next_dst_y_; }
    int dst_height() const noexcept { return dst_h_; }
    uint64_t ring_bytes() const noexcept {
        return static_cast<uint64_t>(row_ring_.size()) * sizeof(float);
    }

private:
    struct VarTapSpan {
        int start_idx = 0;
        int count = 0;
        int weight_offset = 0;
    };

    int src_w_ = 0;
    int src_h_ = 0;
    uint8_t* dst_rgb_ = nullptr;
    int dst_w_ = 0;
    int dst_h_ = 0;

    std::vector<VarTapSpan> x_spans_;
    std::vector<float> x_weights_;
    std::vector<VarTapSpan> y_spans_;
    std::vector<float> y_weights_;

    int ring_slots_ = 0;
    size_t row_floats_ = 0;
    std::vector<float> row_ring_;
    std::vector<int> slot_sy_;

    int pushed_src_rows_ = 0;
    int next_dst_y_ = 0;
    int last_reported_pct_ = -1;
    std::function<void(int)> progress_cb_;

    void compute_horizontal_row(const uint8_t* src_row, int sy);
};

// Full-buffer convenience wrapper around StreamingLanczos3Resizer (works for both upscale and downscale).
bool resize_rgb_lanczos3_streaming(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pct_0_to_100)>& progress_cb = nullptr
);

// Backwards-compatible alias
inline bool upscale_rgb_lanczos3_streaming(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    uint8_t* dst_rgb,
    int dst_w,
    int dst_h,
    const std::atomic<bool>& cancel_flag,
    const std::function<void(int pct_0_to_100)>& progress_cb = nullptr
) {
    return resize_rgb_lanczos3_streaming(
        src_rgb, src_w, src_h, dst_rgb, dst_w, dst_h, cancel_flag, progress_cb
    );
}

// Fast Catmull-Rom (a = -0.5) bicubic sub-pixel ROI resampler on RGBA_8888 bitmaps.
// Maps exact fractional sub-rectangle [roi_x0, roi_x1] x [roi_y0, roi_y1] in `src_rgba`
// onto `dst_rgba` (dst_w x dst_h), used for true bicubic Original comparison tiles at any zoom level.
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
);

} // namespace enhancer

#endif // ENHANCER_RESIZER_H
