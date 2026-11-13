#ifndef ENHANCER_FACE_RESTORER_H
#define ENHANCER_FACE_RESTORER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#else
struct AAssetManager;
#endif

namespace enhancer {

struct NormalizedFace {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float prob = 0.0f;
    // 5-point canonical facial landmarks normalized to [0..1] in source/output aspect space:
    // 0: left eye, 1: right eye, 2: nose tip, 3: left mouth corner, 4: right mouth corner
    float lm_x[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float lm_y[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

// Detects faces + 5-point landmarks on compact work_rgb_buf using SCRFD-2.5G-KPS.
// Runs lightweight 640px inference (< 8 MB scratch) BEFORE output frame allocation.
std::vector<NormalizedFace> detect_faces_scrfd(
    AAssetManager* asset_manager,
    const uint8_t* rgb,
    int width,
    int height,
    const std::string& fallback_models_dir = ""
);

// Restores detected faces in-place on upscaled `final_rgb` using sequential CodeFormer 512x512
// encoder -> codebook argmax -> controllable SFT generator, Oklab skin/monochrome color matching,
// runtime anti-gray/NaN verification (with automatic Vulkan -> CPU retry), and cosine-feathered
// inverse-similarity paste-back. Returns number of successfully restored faces.
int restore_faces_codeformer(
    AAssetManager* asset_manager,
    uint8_t* final_rgb,
    int out_w,
    int out_h,
    const std::vector<NormalizedFace>& faces,
    float face_strength,
    bool allow_vulkan,
    bool disable_fp16,
    int cpu_threads,
    const std::atomic<bool>& cancelled,
    const std::function<void(int done, int total)>& face_progress_cb = nullptr,
    const std::string& fallback_models_dir = "",
    int gpu_index = -1
);

} // namespace enhancer

#endif // ENHANCER_FACE_RESTORER_H
