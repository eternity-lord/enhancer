#include "face_restorer.h"

#include "allocator.h"
#include "cpu.h"
#include "gpu.h"
#include "net.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define FACE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "EnhancerFaces", __VA_ARGS__)
#define FACE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "EnhancerFaces", __VA_ARGS__)
#else
#define FACE_LOGI(...) do { std::fprintf(stderr, "[EnhancerFaces] "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define FACE_LOGW(...) do { std::fprintf(stderr, "[EnhancerFaces WARN] "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif

namespace enhancer {

namespace {

struct Point2f {
    float x = 0.0f;
    float y = 0.0f;
};

struct RawFaceProposal {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float prob = 0.0f;
    Point2f lm[5];
};

struct Affine2D {
    double m[6];
};

float iou_face(const RawFaceProposal& a, const RawFaceProposal& b) {
    const float ix0 = std::max(a.x0, b.x0);
    const float iy0 = std::max(a.y0, b.y0);
    const float ix1 = std::min(a.x1, b.x1);
    const float iy1 = std::min(a.y1, b.y1);
    const float iw = std::max(0.0f, ix1 - ix0);
    const float ih = std::max(0.0f, iy1 - iy0);
    const float inter = iw * ih;
    const float ua = (a.x1 - a.x0) * (a.y1 - a.y0) +
                     (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
    return (ua > 1e-6f) ? (inter / ua) : 0.0f;
}

Affine2D estimate_similarity_5pt(const Point2f src[5], const Point2f dst[5]) {
    double sx = 0.0, sy = 0.0, dx = 0.0, dy = 0.0;
    for (int i = 0; i < 5; ++i) {
        sx += src[i].x;
        sy += src[i].y;
        dx += dst[i].x;
        dy += dst[i].y;
    }
    sx /= 5.0;
    sy /= 5.0;
    dx /= 5.0;
    dy /= 5.0;

    double num_a = 0.0, num_b = 0.0, den = 0.0;
    for (int i = 0; i < 5; ++i) {
        const double x = src[i].x - sx;
        const double y = src[i].y - sy;
        const double u = dst[i].x - dx;
        const double v = dst[i].y - dy;
        num_a += x * u + y * v;
        num_b += x * v - y * u;
        den += x * x + y * y;
    }
    const double a = (den > 1e-9) ? (num_a / den) : 1.0;
    const double b = (den > 1e-9) ? (num_b / den) : 0.0;
    const double tx = dx - (a * sx - b * sy);
    const double ty = dy - (b * sx + a * sy);
    return Affine2D{{a, -b, tx, b, a, ty}};
}

Affine2D invert_affine(const Affine2D& A) {
    const double det = A.m[0] * A.m[4] - A.m[1] * A.m[3];
    const double inv_det = (std::fabs(det) > 1e-12) ? (1.0 / det) : 0.0;
    const double i0 = A.m[4] * inv_det;
    const double i1 = -A.m[1] * inv_det;
    const double i3 = -A.m[3] * inv_det;
    const double i4 = A.m[0] * inv_det;
    const double i2 = -(i0 * A.m[2] + i1 * A.m[5]);
    const double i5 = -(i3 * A.m[2] + i4 * A.m[5]);
    return Affine2D{{i0, i1, i2, i3, i4, i5}};
}

inline float cubic_weight(float x) {
    constexpr float a = -0.5f; // Catmull-Rom
    x = std::fabs(x);
    if (x <= 1.0f) return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
    if (x < 2.0f)  return (((x - 5.0f) * x + 8.0f) * x - 4.0f) * a;
    return 0.0f;
}

inline int reflect_index(int idx, int lim) {
    if (lim <= 1) return 0;
    while (idx < 0 || idx >= lim) {
        if (idx < 0) idx = -idx;
        if (idx >= lim) idx = 2 * lim - 2 - idx;
    }
    return idx;
}

inline void sample_rgb_bicubic_reflect(
    const uint8_t* src,
    int w,
    int h,
    float fx,
    float fy,
    float out_rgb[3]
) {
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    const float dx = fx - static_cast<float>(ix);
    const float dy = fy - static_cast<float>(iy);
    float wx[4], wy[4];
    for (int k = 0; k < 4; ++k) {
        wx[k] = cubic_weight(dx - static_cast<float>(k - 1));
        wy[k] = cubic_weight(dy - static_cast<float>(k - 1));
    }
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;
    for (int ky = 0; ky < 4; ++ky) {
        const int sy = reflect_index(iy + ky - 1, h);
        for (int kx = 0; kx < 4; ++kx) {
            const int sx = reflect_index(ix + kx - 1, w);
            const float wgt = wx[kx] * wy[ky];
            const uint8_t* p = src + (static_cast<size_t>(sy) * w + sx) * 3u;
            out_rgb[0] += wgt * static_cast<float>(p[0]);
            out_rgb[1] += wgt * static_cast<float>(p[1]);
            out_rgb[2] += wgt * static_cast<float>(p[2]);
        }
    }
}

bool read_asset_or_file_text(
    AAssetManager* mgr,
    const std::string& asset_rel_path,
    const std::string& fallback_dir,
    std::string& out_text
) {
#ifdef __ANDROID__
    if (mgr != nullptr) {
        AAsset* asset = AAssetManager_open(mgr, asset_rel_path.c_str(), AASSET_MODE_BUFFER);
        if (asset != nullptr) {
            const off_t len = AAsset_getLength(asset);
            if (len > 0) {
                out_text.resize(static_cast<size_t>(len));
                const int nr = AAsset_read(asset, &out_text[0], static_cast<size_t>(len));
                AAsset_close(asset);
                if (nr == len) {
                    return true;
                }
            } else {
                AAsset_close(asset);
            }
        }
    }
#else
    (void)mgr;
#endif
    if (!fallback_dir.empty()) {
        const std::string full = fallback_dir + "/" + asset_rel_path;
        std::ifstream ifs(full, std::ios::binary);
        if (ifs) {
            std::ostringstream oss;
            oss << ifs.rdbuf();
            out_text = oss.str();
            return !out_text.empty();
        }
    }
    return false;
}

bool load_net_from_asset_or_dir(
    ncnn::Net& net,
    AAssetManager* mgr,
    const std::string& param_rel,
    const std::string& bin_rel,
    const std::string& fallback_dir,
    const std::string* custom_param_mem = nullptr
) {
    int prc = -1;
    if (custom_param_mem != nullptr) {
        prc = net.load_param_mem(custom_param_mem->c_str());
    } else {
#ifdef __ANDROID__
        if (mgr != nullptr) {
            prc = net.load_param(mgr, param_rel.c_str());
        }
#endif
        if (prc != 0 && !fallback_dir.empty()) {
            prc = net.load_param((fallback_dir + "/" + param_rel).c_str());
        }
    }
    if (prc != 0) {
        FACE_LOGW("Failed loading param %s (rc=%d)", param_rel.c_str(), prc);
        return false;
    }

    int mrc = -1;
#ifdef __ANDROID__
    if (mgr != nullptr) {
        mrc = net.load_model(mgr, bin_rel.c_str());
    }
#else
    (void)mgr;
#endif
    if (mrc != 0 && !fallback_dir.empty()) {
        mrc = net.load_model((fallback_dir + "/" + bin_rel).c_str());
    }
    if (mrc != 0) {
        FACE_LOGW("Failed loading bin %s (rc=%d)", bin_rel.c_str(), mrc);
        return false;
    }
    return true;
}

std::string patch_generator_fidelity(std::string param_txt, float w) {
    char repl[64];
    std::snprintf(repl, sizeof(repl), "-23301=2,1.000000e+00,%.6e", w);
    const std::string needle = "-23301=2,1.000000e+00,7.000000e-01";
    size_t pos = 0;
    while ((pos = param_txt.find(needle, pos)) != std::string::npos) {
        param_txt.replace(pos, needle.size(), repl);
        pos += std::strlen(repl);
    }
    return param_txt;
}

inline float srgb_to_linear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.04045f) ? (c * (1.0f / 12.92f)) : std::pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

inline float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}

inline void rgb_u8_to_oklab(const uint8_t* px, float& L, float& A, float& B) {
    const float r = srgb_to_linear(static_cast<float>(px[0]) * (1.0f / 255.0f));
    const float g = srgb_to_linear(static_cast<float>(px[1]) * (1.0f / 255.0f));
    const float b = srgb_to_linear(static_cast<float>(px[2]) * (1.0f / 255.0f));

    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    const float l_ = std::cbrt(std::max(0.0f, l));
    const float m_ = std::cbrt(std::max(0.0f, m));
    const float s_ = std::cbrt(std::max(0.0f, s));

    L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    A = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    B = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

inline void oklab_to_rgb_u8(float L, float A, float B, uint8_t* out_px) {
    const float l_ = L + 0.3963377774f * A + 0.2158037573f * B;
    const float m_ = L - 0.1055613458f * A - 0.0638541728f * B;
    const float s_ = L - 0.0894841775f * A - 1.2914855480f * B;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const float r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    out_px[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(linear_to_srgb(r) * 255.0f + 0.5f), 0, 255));
    out_px[1] = static_cast<uint8_t>(std::clamp(static_cast<int>(linear_to_srgb(g) * 255.0f + 0.5f), 0, 255));
    out_px[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(linear_to_srgb(b) * 255.0f + 0.5f), 0, 255));
}

// Harmonizes CodeFormer's 512x512 output skin tone & brightness with the source patch in Oklab,
// and strictly locks monochrome/sepia chroma so vintage B&W portraits never get pink face ovals.
void harmonize_face_oklab(const uint8_t* src_512, uint8_t* cf_512) {
    double sL = 0.0, sA = 0.0, sB = 0.0, sL2 = 0.0, sA2 = 0.0, sB2 = 0.0;
    double cL = 0.0, cA = 0.0, cB = 0.0, cL2 = 0.0, cA2 = 0.0, cB2 = 0.0;
    uint64_t count = 0;

    // Sample inner facial oval (excludes background hair/sky corners)
    for (int y = 96; y < 448; y += 2) {
        const float ey = (static_cast<float>(y) - 280.0f) / 180.0f;
        for (int x = 96; x < 416; x += 2) {
            const float ex = (static_cast<float>(x) - 256.0f) / 150.0f;
            if (ex * ex + ey * ey > 1.0f) continue;

            const size_t off = (static_cast<size_t>(y) * 512u + static_cast<size_t>(x)) * 3u;
            float sl, sa, sb, cl, ca, cb;
            rgb_u8_to_oklab(src_512 + off, sl, sa, sb);
            rgb_u8_to_oklab(cf_512 + off, cl, ca, cb);

            sL += sl; sA += sa; sB += sb;
            sL2 += sl * sl; sA2 += sa * sa; sB2 += sb * sb;
            cL += cl; cA += ca; cB += cb;
            cL2 += cl * cl; cA2 += ca * ca; cB2 += cb * cb;
            ++count;
        }
    }
    if (count < 64) return;

    const double inv = 1.0 / static_cast<double>(count);
    const float m_sL = static_cast<float>(sL * inv);
    const float m_sA = static_cast<float>(sA * inv);
    const float m_sB = static_cast<float>(sB * inv);
    const float m_cL = static_cast<float>(cL * inv);
    const float m_cA = static_cast<float>(cA * inv);
    const float m_cB = static_cast<float>(cB * inv);

    const float sd_sL = static_cast<float>(std::sqrt(std::max(1e-6, sL2 * inv - m_sL * m_sL)));
    const float sd_sA = static_cast<float>(std::sqrt(std::max(1e-8, sA2 * inv - m_sA * m_sA)));
    const float sd_sB = static_cast<float>(std::sqrt(std::max(1e-8, sB2 * inv - m_sB * m_sB)));
    const float sd_cL = static_cast<float>(std::sqrt(std::max(1e-6, cL2 * inv - m_cL * m_cL)));
    const float sd_cA = static_cast<float>(std::sqrt(std::max(1e-8, cA2 * inv - m_cA * m_cA)));
    const float sd_cB = static_cast<float>(std::sqrt(std::max(1e-8, cB2 * inv - m_cB * m_cB)));

    // Detect B&W or uniform sepia source face: Oklab chroma std < 0.0085 AND chroma mag < 0.032
    const float src_chroma_std = std::hypot(sd_sA, sd_sB);
    const float src_chroma_mag = std::hypot(m_sA, m_sB);
    const bool is_monochrome_or_sepia = (src_chroma_std < 0.0085f && src_chroma_mag < 0.035f);

    const float gain_L = std::clamp(sd_sL / sd_cL, 0.80f, 1.22f);
    const float blend_L = 0.78f;

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < 512 * 512; ++i) {
        uint8_t* px = cf_512 + static_cast<size_t>(i) * 3u;
        float L, A, B;
        rgb_u8_to_oklab(px, L, A, B);

        const float matched_L = m_sL + (L - m_cL) * (1.0f + 0.55f * (gain_L - 1.0f));
        const float out_L = L + blend_L * (matched_L - L);

        float out_A = A;
        float out_B = B;
        if (is_monochrome_or_sepia) {
            // Strictly lock monochrome / archival sepia tone (zero pink skin hallucination!)
            const float ga = std::clamp(sd_sA / std::max(1e-4f, sd_cA), 0.0f, 1.0f);
            const float gb = std::clamp(sd_sB / std::max(1e-4f, sd_cB), 0.0f, 1.0f);
            out_A = m_sA + (A - m_cA) * ga;
            out_B = m_sB + (B - m_cB) * gb;
        } else {
            const float ga = std::clamp(sd_sA / std::max(1e-4f, sd_cA), 0.68f, 1.32f);
            const float gb = std::clamp(sd_sB / std::max(1e-4f, sd_cB), 0.68f, 1.32f);
            const float target_A = m_sA + (A - m_cA) * (1.0f + 0.60f * (ga - 1.0f));
            const float target_B = m_sB + (B - m_cB) * (1.0f + 0.60f * (gb - 1.0f));
            out_A = A + 0.84f * (target_A - A);
            out_B = B + 0.84f * (target_B - B);
        }

        oklab_to_rgb_u8(out_L, out_A, out_B, px);
    }
}

// Verifies CodeFormer output tensor is finite, non-gray, and structurally coherent with source patch.
bool verify_and_unpack_codeformer_out(
    const ncnn::Mat& gen_out,
    const uint8_t* src_512,
    std::vector<uint8_t>& out_512
) {
    if (gen_out.empty() || gen_out.w != 512 || gen_out.h != 512 || gen_out.c < 3) {
        return false;
    }
    out_512.resize(512u * 512u * 3u);
    const float* rch = gen_out.channel(0);
    const float* gch = gen_out.channel(1);
    const float* bch = gen_out.channel(2);

    double sum_y = 0.0, sum_y2 = 0.0, sum_abs_diff = 0.0;
    constexpr int kPixels = 512 * 512;
    for (int i = 0; i < kPixels; ++i) {
        const float rf = rch[i];
        const float gf = gch[i];
        const float bf = bch[i];
        if (!std::isfinite(rf) || !std::isfinite(gf) || !std::isfinite(bf)) {
            FACE_LOGW("CodeFormer output contained NaN/Inf at pixel %d", i);
            return false;
        }
        const float rv = (std::clamp(rf, -1.0f, 1.0f) + 1.0f) * 127.5f;
        const float gv = (std::clamp(gf, -1.0f, 1.0f) + 1.0f) * 127.5f;
        const float bv = (std::clamp(bf, -1.0f, 1.0f) + 1.0f) * 127.5f;

        const uint8_t ru = static_cast<uint8_t>(std::clamp(static_cast<int>(rv + 0.5f), 0, 255));
        const uint8_t gu = static_cast<uint8_t>(std::clamp(static_cast<int>(gv + 0.5f), 0, 255));
        const uint8_t bu = static_cast<uint8_t>(std::clamp(static_cast<int>(bv + 0.5f), 0, 255));

        out_512[static_cast<size_t>(i) * 3u + 0] = ru;
        out_512[static_cast<size_t>(i) * 3u + 1] = gu;
        out_512[static_cast<size_t>(i) * 3u + 2] = bu;

        const double y = 0.299 * ru + 0.587 * gu + 0.114 * bu;
        sum_y += y;
        sum_y2 += y * y;
        sum_abs_diff += (std::fabs(rv - src_512[i * 3 + 0]) +
                         std::fabs(gv - src_512[i * 3 + 1]) +
                         std::fabs(bv - src_512[i * 3 + 2])) * (1.0 / 3.0);
    }

    const double mean_y = sum_y / static_cast<double>(kPixels);
    const double std_y = std::sqrt(std::max(0.0, sum_y2 / static_cast<double>(kPixels) - mean_y * mean_y));
    const double mean_diff = sum_abs_diff / static_cast<double>(kPixels);

    if (std_y < 3.5 || mean_diff > 90.0) {
        FACE_LOGW("CodeFormer output failed sanity check (std_y=%.2f, mean_diff=%.2f)", std_y, mean_diff);
        return false;
    }
    return true;
}

struct EncodedFaceLatents {
    ncnn::Mat ef32;
    ncnn::Mat ef64;
    ncnn::Mat ef128;
    ncnn::Mat ef256;
    ncnn::Mat lq_feat;
    ncnn::Mat min_enc;
    bool valid = false;
};

ncnn::Option make_codeformer_option(
    bool use_vulkan,
    bool disable_fp16,
    int cpu_threads,
    ncnn::PoolAllocator* blob_alloc,
    ncnn::PoolAllocator* work_alloc
) {
    ncnn::Option opt;
    opt.lightmode = true;
    opt.num_threads = std::clamp(cpu_threads, 1, 4);
    opt.blob_allocator = blob_alloc;
    opt.workspace_allocator = work_alloc;
    opt.use_vulkan_compute = use_vulkan;
    opt.use_cooperative_matrix = false;

    if (use_vulkan) {
        opt.use_fp16_packed = !disable_fp16;
        opt.use_fp16_storage = !disable_fp16;
        opt.use_fp16_arithmetic = false;
        opt.use_shader_local_memory = true;
    } else {
        // Essential on CPU: disabling Winograd & SGEMM prevents 3.9 GB workspace explosion on 512x512x256
        opt.use_winograd_convolution = false;
        opt.use_sgemm_convolution = false;
        opt.use_fp16_packed = true;
        opt.use_fp16_storage = true;
        opt.use_fp16_arithmetic = false;
    }
    return opt;
}

bool run_codeformer_batch_once(
    AAssetManager* asset_manager,
    const std::string& fallback_models_dir,
    const std::string& patched_gen_param,
    const std::vector<std::vector<uint8_t>>& aligned_patches,
    bool use_vulkan,
    bool disable_fp16,
    int cpu_threads,
    const std::atomic<bool>& cancelled,
    std::vector<std::vector<uint8_t>>& restored_patches_out
) {
    const size_t n_faces = aligned_patches.size();
    restored_patches_out.assign(n_faces, {});
    if (n_faces == 0) return true;

    // Thread-safe PoolAllocator is mandatory because CodeFormer MultiHeadAttention allocates
    // per-head workspace inside OpenMP parallel loops.
    ncnn::PoolAllocator blob_alloc;
    ncnn::PoolAllocator work_alloc;
    blob_alloc.set_size_compare_ratio(0.0f);
    work_alloc.set_size_compare_ratio(0.0f);

    std::vector<EncodedFaceLatents> latents(n_faces);

    // Phase 1: Scoped Encoder pass (destroys encoder Net before loading generator Net!)
    {
        ncnn::Net enc;
        enc.opt = make_codeformer_option(use_vulkan, disable_fp16, cpu_threads, &blob_alloc, &work_alloc);
        if (!load_net_from_asset_or_dir(
                enc,
                asset_manager,
                "models/codeformer_encoder.param",
                "models/codeformer_encoder.bin",
                fallback_models_dir
            )) {
            return false;
        }

        for (size_t i = 0; i < n_faces; ++i) {
            if (cancelled.load()) return false;
            ncnn::Mat in = ncnn::Mat::from_pixels(
                aligned_patches[i].data(),
                ncnn::Mat::PIXEL_RGB,
                512,
                512
            );
            const float mean_v[3] = {127.5f, 127.5f, 127.5f};
            const float norm_v[3] = {1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f};
            in.substract_mean_normalize(mean_v, norm_v);

            ncnn::Extractor ex = enc.create_extractor();
            ex.input("input", in);

            ncnn::Mat t32, t64, t128, t256, tlq, tsoh;
            if (ex.extract("enc_feat_32", t32) != 0 ||
                ex.extract("enc_feat_64", t64) != 0 ||
                ex.extract("enc_feat_128", t128) != 0 ||
                ex.extract("enc_feat_256", t256) != 0 ||
                ex.extract("lq_feat", tlq) != 0 ||
                ex.extract("soft_one_hot", tsoh) != 0 ||
                tsoh.w != 1024 || tsoh.h != 256) {
                FACE_LOGW(
                    "CodeFormer encoder extraction failed on face %zu (vulkan=%d, soh=%dx%d)",
                    i,
                    use_vulkan ? 1 : 0,
                    tsoh.w,
                    tsoh.h
                );
                return false;
            }

            // Clone to default heap BEFORE Extractor/Net destructors & PoolAllocator::clear()!
            latents[i].ef32 = t32.clone();
            latents[i].ef64 = t64.clone();
            latents[i].ef128 = t128.clone();
            latents[i].ef256 = t256.clone();
            latents[i].lq_feat = tlq.clone();

            ncnn::Mat soh_copy = tsoh.clone();
            ncnn::Mat min_enc(1024, 256);
            min_enc.fill(0.0f);
            for (int row_idx = 0; row_idx < soh_copy.h; ++row_idx) {
                const float* rptr = soh_copy.row(row_idx);
                int best_k = 0;
                float best_v = rptr[0];
                for (int k = 1; k < soh_copy.w; ++k) {
                    if (!std::isfinite(rptr[k])) return false;
                    if (rptr[k] > best_v) {
                        best_v = rptr[k];
                        best_k = k;
                    }
                }
                min_enc.row(row_idx)[best_k] = 1.0f;
            }
            latents[i].min_enc = min_enc;
            latents[i].valid = true;
        }
    }
    blob_alloc.clear();
    work_alloc.clear();

    if (cancelled.load()) {
        return false;
    }

    // Phase 2: Scoped Generator pass with controllable SFT fidelity w
    {
        ncnn::Net gen;
        gen.opt = make_codeformer_option(use_vulkan, disable_fp16, cpu_threads, &blob_alloc, &work_alloc);
        if (!load_net_from_asset_or_dir(
                gen,
                asset_manager,
                "models/codeformer_generator.param",
                "models/codeformer_generator.bin",
                fallback_models_dir,
                &patched_gen_param
            )) {
            return false;
        }

        for (size_t i = 0; i < n_faces; ++i) {
            if (cancelled.load()) return false;
            if (!latents[i].valid) return false;

            ncnn::Extractor ex = gen.create_extractor();
            ex.input("enc_feat_32", latents[i].ef32);
            ex.input("enc_feat_64", latents[i].ef64);
            ex.input("enc_feat_128", latents[i].ef128);
            ex.input("enc_feat_256", latents[i].ef256);
            ex.input("style_feat", latents[i].lq_feat);
            ex.input("input", latents[i].min_enc);

            ncnn::Mat t_out;
            if (ex.extract("out", t_out) != 0) {
                FACE_LOGW("CodeFormer generator extract('out') failed on face %zu (vulkan=%d)", i, use_vulkan ? 1 : 0);
                return false;
            }
            ncnn::Mat gen_out = t_out.clone();

            // Release per-face encoder latents immediately
            latents[i] = EncodedFaceLatents{};

            if (!verify_and_unpack_codeformer_out(gen_out, aligned_patches[i].data(), restored_patches_out[i])) {
                return false;
            }
        }
    }
    blob_alloc.clear();
    work_alloc.clear();

    return true;
}

} // namespace

std::vector<NormalizedFace> detect_faces_scrfd(
    AAssetManager* asset_manager,
    const uint8_t* rgb,
    int width,
    int height,
    const std::string& fallback_models_dir
) {
    std::vector<NormalizedFace> result;
    if (!rgb || width < 24 || height < 24) {
        return result;
    }

    ncnn::PoolAllocator blob_alloc;
    ncnn::PoolAllocator work_alloc;

    std::vector<RawFaceProposal> proposals;
    int wpad = 0;
    int hpad = 0;
    float scale = 1.0f;

    {
        ncnn::Net net;
        net.opt.lightmode = true;
        net.opt.num_threads = 2;
        net.opt.use_vulkan_compute = false; // 1.6 MB SCRFD runs in ~20ms on CPU with zero GPU contention
        net.opt.blob_allocator = &blob_alloc;
        net.opt.workspace_allocator = &work_alloc;

        if (!load_net_from_asset_or_dir(
                net,
                asset_manager,
                "models/scrfd_2.5g_kps.param",
                "models/scrfd_2.5g_kps.bin",
                fallback_models_dir
            )) {
            return result;
        }

        constexpr int kTargetSize = 640;
        int w = width;
        int h = height;
        if (w > h) {
            scale = static_cast<float>(kTargetSize) / static_cast<float>(w);
            w = kTargetSize;
            h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));
        } else {
            scale = static_cast<float>(kTargetSize) / static_cast<float>(h);
            h = kTargetSize;
            w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
        }

        ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb, ncnn::Mat::PIXEL_RGB, width, height, w, h);
        wpad = (w + 31) / 32 * 32 - w;
        hpad = (h + 31) / 32 * 32 - h;
        ncnn::Mat in_pad;
        ncnn::copy_make_border(
            in,
            in_pad,
            hpad / 2,
            hpad - hpad / 2,
            wpad / 2,
            wpad - wpad / 2,
            ncnn::BORDER_CONSTANT,
            0.0f
        );
        const float mean_vals[3] = {127.5f, 127.5f, 127.5f};
        const float norm_vals[3] = {1.0f / 128.0f, 1.0f / 128.0f, 1.0f / 128.0f};
        in_pad.substract_mean_normalize(mean_vals, norm_vals);

        ncnn::Extractor ex = net.create_extractor();
        ex.input("input.1", in_pad);

        constexpr int kStrides[3] = {8, 16, 32};
        const char* kScoreNames[3] = {"score_8", "score_16", "score_32"};
        const char* kBboxNames[3] = {"bbox_8", "bbox_16", "bbox_32"};
        const char* kKpsNames[3] = {"kps_8", "kps_16", "kps_32"};
        constexpr float kProbThresh = 0.44f;

        for (int si = 0; si < 3; ++si) {
            ncnn::Mat score_blob, bbox_blob, kps_blob;
            if (ex.extract(kScoreNames[si], score_blob) != 0 ||
                ex.extract(kBboxNames[si], bbox_blob) != 0 ||
                ex.extract(kKpsNames[si], kps_blob) != 0) {
                continue;
            }
            const int stride = kStrides[si];
            const int gw = score_blob.w;
            const int gh = score_blob.h;
            for (int q = 0; q < 2; ++q) {
                const ncnn::Mat sc = score_blob.channel(q);
                const ncnn::Mat bb = bbox_blob.channel_range(q * 4, 4);
                const ncnn::Mat kp = kps_blob.channel_range(q * 10, 10);
                for (int i = 0; i < gh; ++i) {
                    for (int j = 0; j < gw; ++j) {
                        const int idx = i * gw + j;
                        const float prob = sc[idx];
                        if (prob >= kProbThresh) {
                            const float cx = static_cast<float>(j * stride);
                            const float cy = static_cast<float>(i * stride);
                            RawFaceProposal p;
                            p.x0 = cx - bb.channel(0)[idx] * stride;
                            p.y0 = cy - bb.channel(1)[idx] * stride;
                            p.x1 = cx + bb.channel(2)[idx] * stride;
                            p.y1 = cy + bb.channel(3)[idx] * stride;
                            p.prob = prob;
                            for (int m = 0; m < 5; ++m) {
                                p.lm[m].x = cx + kp.channel(m * 2)[idx] * stride;
                                p.lm[m].y = cy + kp.channel(m * 2 + 1)[idx] * stride;
                            }
                            proposals.push_back(p);
                        }
                    }
                }
            }
        }
    }
    blob_alloc.clear();
    work_alloc.clear();

    std::sort(proposals.begin(), proposals.end(), [](const RawFaceProposal& a, const RawFaceProposal& b) {
        return a.prob > b.prob;
    });

    constexpr float kNmsThresh = 0.40f;
    std::vector<RawFaceProposal> kept_raw;
    for (const auto& p : proposals) {
        bool keep = true;
        for (const auto& k : kept_raw) {
            if (iou_face(p, k) > kNmsThresh) {
                keep = false;
                break;
            }
        }
        if (keep) {
            kept_raw.push_back(p);
        }
    }

    const float pad_x = static_cast<float>(wpad / 2);
    const float pad_y = static_cast<float>(hpad / 2);
    const float inv_w = 1.0f / static_cast<float>(width);
    const float inv_h = 1.0f / static_cast<float>(height);

    for (const auto& p : kept_raw) {
        const float x0 = std::clamp((p.x0 - pad_x) / scale, 0.0f, static_cast<float>(width));
        const float y0 = std::clamp((p.y0 - pad_y) / scale, 0.0f, static_cast<float>(height));
        const float x1 = std::clamp((p.x1 - pad_x) / scale, 0.0f, static_cast<float>(width));
        const float y1 = std::clamp((p.y1 - pad_y) / scale, 0.0f, static_cast<float>(height));
        const float bw = x1 - x0;
        const float bh = y1 - y0;
        if (bw < 18.0f || bh < 18.0f) {
            continue;
        }

        Point2f lm_px[5];
        for (int m = 0; m < 5; ++m) {
            lm_px[m].x = (p.lm[m].x - pad_x) / scale;
            lm_px[m].y = (p.lm[m].y - pad_y) / scale;
        }
        const float eye_dist = std::hypot(lm_px[1].x - lm_px[0].x, lm_px[1].y - lm_px[0].y);
        if (eye_dist < 6.5f) {
            continue;
        }

        NormalizedFace nf;
        nf.x0 = x0 * inv_w;
        nf.y0 = y0 * inv_h;
        nf.x1 = x1 * inv_w;
        nf.y1 = y1 * inv_h;
        nf.prob = p.prob;
        for (int m = 0; m < 5; ++m) {
            nf.lm_x[m] = lm_px[m].x * inv_w;
            nf.lm_y[m] = lm_px[m].y * inv_h;
        }
        result.push_back(nf);
    }

    // Prioritize largest prominent portraits first (cap at 6 faces per photo)
    std::sort(result.begin(), result.end(), [](const NormalizedFace& a, const NormalizedFace& b) {
        const float area_a = (a.x1 - a.x0) * (a.y1 - a.y0) * a.prob;
        const float area_b = (b.x1 - b.x0) * (b.y1 - b.y0) * b.prob;
        return area_a > area_b;
    });
    if (result.size() > 6) {
        result.resize(6);
    }

    FACE_LOGI("SCRFD detected %zu valid face(s) on %dx%d", result.size(), width, height);
    return result;
}

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
    const std::function<void(int done, int total)>& face_progress_cb,
    const std::string& fallback_models_dir
) {
    if (!final_rgb || out_w < 64 || out_h < 64 || faces.empty() || face_strength <= 0.02f) {
        return 0;
    }

    const float strength = std::clamp(face_strength, 0.0f, 1.0f);
    // CodeFormer SFT fidelity weight w: lower w = stronger codebook synthesis, higher w = closer to LQ input
    const float cf_fidelity_w = std::clamp(0.95f - 0.50f * strength, 0.45f, 0.92f);
    const float blend_gain = std::clamp(0.35f + 0.65f * (strength / 0.75f), 0.25f, 1.0f);

    std::string raw_gen_param;
    if (!read_asset_or_file_text(
            asset_manager,
            "models/codeformer_generator.param",
            fallback_models_dir,
            raw_gen_param
        )) {
        FACE_LOGW("Could not read models/codeformer_generator.param; skipping face restoration");
        return 0;
    }
    const std::string patched_gen_param = patch_generator_fidelity(raw_gen_param, cf_fidelity_w);

    static const Point2f kFfhqTemplate512[5] = {
        {192.98138f, 239.94708f},
        {318.90277f, 240.19360f},
        {256.63416f, 314.01935f},
        {201.26117f, 371.41043f},
        {313.08905f, 371.15118f}
    };

    const int total_faces = static_cast<int>(faces.size());
    int restored_count = 0;

    // Process in mini-batches of up to 2 faces so encoder & generator never coexist in RAM
    // AND latent feature memory stays < 30 MB even on group photos.
    constexpr size_t kMiniBatch = 2;
    for (size_t start = 0; start < faces.size(); start += kMiniBatch) {
        if (cancelled.load()) {
            break;
        }
        const size_t batch_n = std::min(kMiniBatch, faces.size() - start);
        std::vector<Affine2D> m_out_to_512(batch_n);
        std::vector<Affine2D> m_512_to_out(batch_n);
        std::vector<std::vector<uint8_t>> aligned_patches(batch_n);

        for (size_t bi = 0; bi < batch_n; ++bi) {
            const NormalizedFace& nf = faces[start + bi];
            Point2f dst_lm[5];
            for (int m = 0; m < 5; ++m) {
                dst_lm[m].x = nf.lm_x[m] * static_cast<float>(out_w);
                dst_lm[m].y = nf.lm_y[m] * static_cast<float>(out_h);
            }
            m_out_to_512[bi] = estimate_similarity_5pt(dst_lm, kFfhqTemplate512);
            m_512_to_out[bi] = invert_affine(m_out_to_512[bi]);

            aligned_patches[bi].resize(512u * 512u * 3u);
            const Affine2D invM = m_512_to_out[bi];
            uint8_t* patch_ptr = aligned_patches[bi].data();

            #pragma omp parallel for schedule(static)
            for (int y = 0; y < 512; ++y) {
                for (int x = 0; x < 512; ++x) {
                    const float sx = static_cast<float>(invM.m[0] * x + invM.m[1] * y + invM.m[2]);
                    const float sy = static_cast<float>(invM.m[3] * x + invM.m[4] * y + invM.m[5]);
                    float c[3];
                    sample_rgb_bicubic_reflect(final_rgb, out_w, out_h, sx, sy, c);
                    uint8_t* p = patch_ptr + (static_cast<size_t>(y) * 512u + x) * 3u;
                    p[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(c[0] + 0.5f), 0, 255));
                    p[1] = static_cast<uint8_t>(std::clamp(static_cast<int>(c[1] + 0.5f), 0, 255));
                    p[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(c[2] + 0.5f), 0, 255));
                }
            }
        }

        std::vector<std::vector<uint8_t>> restored_patches;
        bool ok = false;
        if (allow_vulkan) {
            ok = run_codeformer_batch_once(
                asset_manager,
                fallback_models_dir,
                patched_gen_param,
                aligned_patches,
                /*use_vulkan=*/true,
                disable_fp16,
                cpu_threads,
                cancelled,
                restored_patches
            );
            if (!ok && !cancelled.load()) {
                FACE_LOGW("CodeFormer Vulkan pass failed or drifted; retrying face mini-batch on CPU");
            }
        }
        if (!ok && !cancelled.load()) {
            ok = run_codeformer_batch_once(
                asset_manager,
                fallback_models_dir,
                patched_gen_param,
                aligned_patches,
                /*use_vulkan=*/false,
                /*disable_fp16=*/false,
                cpu_threads,
                cancelled,
                restored_patches
            );
        }

        if (!ok) {
            if (cancelled.load()) break;
            FACE_LOGW("Skipping face mini-batch at index %zu (CodeFormer unavailable)", start);
            continue;
        }

        // Color-match & feather-paste each restored face back onto final_rgb
        for (size_t bi = 0; bi < batch_n; ++bi) {
            if (cancelled.load()) break;
            harmonize_face_oklab(aligned_patches[bi].data(), restored_patches[bi].data());

            const Affine2D& M = m_out_to_512[bi];
            const Affine2D& invM = m_512_to_out[bi];
            const double corners_x[4] = {0.0, 511.0, 511.0, 0.0};
            const double corners_y[4] = {0.0, 0.0, 511.0, 511.0};
            int bx0 = out_w, by0 = out_h, bx1 = 0, by1 = 0;
            for (int c = 0; c < 4; ++c) {
                const int ox = static_cast<int>(std::floor(
                    invM.m[0] * corners_x[c] + invM.m[1] * corners_y[c] + invM.m[2]
                ));
                const int oy = static_cast<int>(std::floor(
                    invM.m[3] * corners_x[c] + invM.m[4] * corners_y[c] + invM.m[5]
                ));
                bx0 = std::min(bx0, ox - 2);
                by0 = std::min(by0, oy - 2);
                bx1 = std::max(bx1, ox + 2);
                by1 = std::max(by1, oy + 2);
            }
            bx0 = std::max(0, bx0);
            by0 = std::max(0, by0);
            bx1 = std::min(out_w - 1, bx1);
            by1 = std::min(out_h - 1, by1);

            const uint8_t* cf_ptr = restored_patches[bi].data();

            #pragma omp parallel for schedule(static) if (by1 - by0 >= 32)
            for (int y = by0; y <= by1; ++y) {
                for (int x = bx0; x <= bx1; ++x) {
                    const float u = static_cast<float>(M.m[0] * x + M.m[1] * y + M.m[2]);
                    const float v = static_cast<float>(M.m[3] * x + M.m[4] * y + M.m[5]);
                    if (u < 16.0f || u > 495.0f || v < 16.0f || v > 495.0f) {
                        continue;
                    }

                    // Anatomical facial ellipse + 48px border guard in canonical 512x512 FFHQ space
                    const float ex = (u - 256.0f) * (1.0f / 192.0f);
                    const float ey = (v - 282.0f) * (1.0f / 218.0f);
                    const float r_ell = std::sqrt(ex * ex + ey * ey);
                    const float w_ell = 1.0f - std::clamp((r_ell - 0.68f) * (1.0f / 0.32f), 0.0f, 1.0f);
                    const float d_box = std::min({u - 16.0f, 495.0f - u, v - 16.0f, 495.0f - v});
                    const float w_box = std::clamp(d_box * (1.0f / 48.0f), 0.0f, 1.0f);

                    float alpha = std::min(w_ell, w_box);
                    alpha = alpha * alpha * (3.0f - 2.0f * alpha) * blend_gain;
                    if (alpha <= 1e-4f) {
                        continue;
                    }

                    float fc[3];
                    sample_rgb_bicubic_reflect(cf_ptr, 512, 512, u, v, fc);
                    uint8_t* dst = final_rgb + (static_cast<size_t>(y) * out_w + x) * 3u;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float blended =
                            static_cast<float>(dst[ch]) * (1.0f - alpha) +
                            std::clamp(fc[ch], 0.0f, 255.0f) * alpha;
                        dst[ch] = static_cast<uint8_t>(std::clamp(static_cast<int>(blended + 0.5f), 0, 255));
                    }
                }
            }

            ++restored_count;
            if (face_progress_cb) {
                face_progress_cb(restored_count, total_faces);
            }
        }
    }

    FACE_LOGI("CodeFormer restored %d / %d face(s) (w=%.2f)", restored_count, total_faces, cf_fidelity_w);
    return restored_count;
}

} // namespace enhancer
