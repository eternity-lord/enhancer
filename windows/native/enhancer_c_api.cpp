#include "enhancer_c_api.h"

#include "enhancer_types.h"
#include "image_io.h"
#include "platform_io.h"
#include "resizer.h"
#include "upscaler.h"

#include "cpu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_cancel_flag{false};
std::atomic<bool> g_seam_cancel_flag{false};
std::mutex g_engine_session_mutex;

void copy_utf8_trunc(char* dst, size_t cap, const std::string& src) noexcept {
    if (!dst || cap == 0) {
        return;
    }
    const size_t n = std::min(cap - 1u, src.size());
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

enhancer::UpscaleConfig map_c_config(const EnhancerCJobConfig& j) {
    enhancer::UpscaleConfig cfg;
    cfg.models_base_dir = j.models_dir_utf8 ? j.models_dir_utf8 : "";
    cfg.param_asset_path = j.param_rel_utf8 ? j.param_rel_utf8 : "models/realesr-general-x4v3.param";
    cfg.bin_asset_path = j.bin_rel_utf8 ? j.bin_rel_utf8 : "models/realesr-general-x4v3.bin";
    cfg.output_file_path = j.output_path_utf8 ? j.output_path_utf8 : "";
    cfg.model_scale = j.model_scale;
    cfg.target_long_side = j.target_long_side;
    cfg.requested_tile_size = j.tile_size;
    cfg.disable_fp16 = (j.disable_fp16 != 0);
    cfg.max_quality = (j.max_quality != 0);
    cfg.tta_x8 = (j.tta_x8 != 0);
    cfg.safe_mode = (j.safe_mode != 0);
    cfg.force_cpu = (j.force_cpu != 0);
    cfg.gpu_index = j.gpu_index;
    cfg.num_threads = j.num_threads;
    cfg.sharpness = std::clamp(static_cast<int>(j.sharpness), 0, 100);
    const int mode_clamped = (j.enhance_mode >= 0 && j.enhance_mode <= 6) ? j.enhance_mode : 2;
    cfg.mode = static_cast<enhancer::EnhanceMode>(mode_clamped);
    cfg.enhance_faces = (j.enhance_faces != 0);
    cfg.face_strength = std::clamp(j.face_strength, 0.0f, 1.0f);
    cfg.light_ai = (j.light_ai != 0);
    cfg.light_ai_strength = std::clamp(j.light_ai_strength, 0.0f, 1.0f);
    cfg.denoise_strength = std::clamp(j.denoise_strength, 0.0f, 1.0f);
    cfg.ultra_intensity = std::clamp(j.ultra_intensity, 0.0f, 1.0f);
    cfg.ultra_size = std::clamp(j.ultra_size, 0.0f, 1.0f);
    cfg.dpi = (j.dpi > 0) ? j.dpi : 300;
    cfg.format = (j.output_format == 1)
        ? enhancer::ImageFormat::PNG
        : enhancer::ImageFormat::JPEG;
    cfg.jpeg_quality = std::clamp(static_cast<int>(j.jpeg_quality), 1, 100);
    cfg.output_fd = -1;
    return cfg;
}

void fill_c_result(const enhancer::UpscaleResult& r, EnhancerCResult* out) noexcept {
    if (!out) {
        return;
    }
    std::memset(out, 0, sizeof(EnhancerCResult));
    out->status_code = static_cast<int32_t>(r.status);
    out->out_width = r.out_width;
    out->out_height = r.out_height;
    out->used_cpu_fallback = r.used_cpu_fallback ? 1 : 0;
    out->final_tile_size = r.final_tile_size;
    out->faces_detected = r.faces_detected;
    out->faces_restored = r.faces_restored;
    copy_utf8_trunc(out->backend_used, sizeof(out->backend_used), r.backend_used);
    copy_utf8_trunc(out->fallback_summary, sizeof(out->fallback_summary), r.fallback_summary);
    copy_utf8_trunc(out->error_detail, sizeof(out->error_detail), r.error_detail);
}

} // namespace

extern "C" {

ENHANCER_API int32_t ENHANCER_CALL enhancer_get_cpu_thread_count(int32_t* out_big_cores) {
    const int total = ncnn::get_cpu_count();
    const int big = ncnn::get_big_cpu_count();
    if (out_big_cores) {
        *out_big_cores = big > 0 ? big : total;
    }
    return total > 0 ? total : 1;
}

ENHANCER_API void ENHANCER_CALL enhancer_get_system_memory_mb(
    int32_t* out_avail_mb,
    int32_t* out_total_mb
) {
    int32_t avail_mb = 4096;
    int32_t total_mb = 8192;

#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(MEMORYSTATUSEX);
    if (::GlobalMemoryStatusEx(&mem)) {
        total_mb = static_cast<int32_t>(
            std::min<unsigned long long>(mem.ullTotalPhys / (1024ull * 1024ull), 2000000ull)
        );
        avail_mb = static_cast<int32_t>(
            std::min<unsigned long long>(mem.ullAvailPhys / (1024ull * 1024ull), 2000000ull)
        );
    }
#else
    std::FILE* fp = std::fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        long long mem_total_kb = 0;
        long long mem_avail_kb = 0;
        while (std::fgets(line, sizeof(line), fp)) {
            if (std::sscanf(line, "MemTotal: %lld kB", &mem_total_kb) == 1) continue;
            if (std::sscanf(line, "MemAvailable: %lld kB", &mem_avail_kb) == 1) continue;
        }
        std::fclose(fp);
        if (mem_total_kb > 0) {
            total_mb = static_cast<int32_t>(mem_total_kb / 1024ll);
        }
        if (mem_avail_kb > 0) {
            avail_mb = static_cast<int32_t>(mem_avail_kb / 1024ll);
        }
    }
#endif

    if (out_avail_mb) {
        *out_avail_mb = std::max<int32_t>(128, avail_mb);
    }
    if (out_total_mb) {
        *out_total_mb = std::max<int32_t>(256, total_mb);
    }
}

ENHANCER_API int32_t ENHANCER_CALL enhancer_enumerate_gpus(
    EnhancerCGpuInfo* out_gpus,
    int32_t max_gpus
) {
    try {
        const std::vector<enhancer::GpuDeviceDesc> gpus = enhancer::enumerate_vulkan_gpus();
        const int32_t total = static_cast<int32_t>(gpus.size());
        if (out_gpus != nullptr && max_gpus > 0) {
            const int32_t count = std::min(total, max_gpus);
            for (int32_t i = 0; i < count; ++i) {
                std::memset(&out_gpus[i], 0, sizeof(EnhancerCGpuInfo));
                out_gpus[i].index = gpus[static_cast<size_t>(i)].index;
                out_gpus[i].vendor_id = gpus[static_cast<size_t>(i)].vendor_id;
                out_gpus[i].type = gpus[static_cast<size_t>(i)].type;
                out_gpus[i].heap_budget_mb = gpus[static_cast<size_t>(i)].heap_budget_mb;
                out_gpus[i].is_recommended = gpus[static_cast<size_t>(i)].is_recommended ? 1 : 0;
                copy_utf8_trunc(
                    out_gpus[i].name,
                    sizeof(out_gpus[i].name),
                    gpus[static_cast<size_t>(i)].name
                );
            }
        }
        return total;
    } catch (...) {
        return 0;
    }
}

ENHANCER_API void ENHANCER_CALL enhancer_get_diagnostics_summary(
    char* out_utf8,
    int32_t out_cap
) {
    if (!out_utf8 || out_cap <= 0) {
        return;
    }
    try {
        const std::string s = enhancer::query_vulkan_diagnostics_summary();
        copy_utf8_trunc(out_utf8, static_cast<size_t>(out_cap), s);
    } catch (...) {
        copy_utf8_trunc(out_utf8, static_cast<size_t>(out_cap), "Diagnostics unavailable");
    }
}

ENHANCER_API int32_t ENHANCER_CALL enhancer_calibrate_fp16_policy(
    const char* models_dir_utf8,
    int32_t* out_vulkan_available,
    int32_t* out_recommend_disable_fp16,
    char* out_reason_utf8,
    int32_t out_reason_cap
) {
    try {
        std::lock_guard<std::mutex> lock(g_engine_session_mutex);
        const std::string dir = models_dir_utf8 ? models_dir_utf8 : "";
        const enhancer::Fp16CalibrationReport rep =
            enhancer::calibrate_vulkan_fp16_policy(nullptr, dir);
        if (out_vulkan_available) {
            *out_vulkan_available = rep.vulkan_available ? 1 : 0;
        }
        if (out_recommend_disable_fp16) {
            *out_recommend_disable_fp16 = rep.recommend_disable_fp16 ? 1 : 0;
        }
        if (out_reason_utf8 && out_reason_cap > 0) {
            copy_utf8_trunc(out_reason_utf8, static_cast<size_t>(out_reason_cap), rep.reason);
        }
        return 0;
    } catch (...) {
        if (out_vulkan_available) *out_vulkan_available = 0;
        if (out_recommend_disable_fp16) *out_recommend_disable_fp16 = 0;
        if (out_reason_utf8 && out_reason_cap > 0) {
            copy_utf8_trunc(out_reason_utf8, static_cast<size_t>(out_reason_cap), "FP16 calibration failed");
        }
        return -1;
    }
}

ENHANCER_API void ENHANCER_CALL enhancer_compute_dimensions(
    int32_t src_w,
    int32_t src_h,
    int32_t model_scale,
    int32_t target_long_side,
    int32_t max_quality,
    int32_t out_plan_9[9]
) {
    if (!out_plan_9) {
        return;
    }
    const enhancer::DimensionPlan p = enhancer::compute_dimensions(
        src_w,
        src_h,
        model_scale,
        target_long_side,
        max_quality != 0
    );
    out_plan_9[0] = p.src_w;
    out_plan_9[1] = p.src_h;
    out_plan_9[2] = p.work_w;
    out_plan_9[3] = p.work_h;
    out_plan_9[4] = p.model_out_w;
    out_plan_9[5] = p.model_out_h;
    out_plan_9[6] = p.final_w;
    out_plan_9[7] = p.final_h;
    out_plan_9[8] = (p.pre_downscaled ? 1 : 0) |
                    (p.post_upscaled ? 2 : 0) |
                    (p.post_downscaled ? 4 : 0);
}

ENHANCER_API void ENHANCER_CALL enhancer_reset_cancel(void) {
    g_cancel_flag.store(false, std::memory_order_relaxed);
}

ENHANCER_API void ENHANCER_CALL enhancer_request_cancel(void) {
    g_cancel_flag.store(true, std::memory_order_relaxed);
}

ENHANCER_API void ENHANCER_CALL enhancer_cancel_seam_test(void) {
    g_seam_cancel_flag.store(true, std::memory_order_relaxed);
}

ENHANCER_API void ENHANCER_CALL enhancer_upscale_rgba(
    const uint8_t* src_rgba,
    int32_t src_w,
    int32_t src_h,
    int32_t src_stride_bytes,
    const EnhancerCJobConfig* job_cfg,
    EnhancerProgressFn progress_fn,
    void* user_data,
    EnhancerCResult* out_result
) {
    if (!out_result) {
        return;
    }
    std::memset(out_result, 0, sizeof(EnhancerCResult));
    if (!src_rgba || !job_cfg) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_INVALID_ARGS);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "Null src_rgba or job_cfg");
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(g_engine_session_mutex);
        g_cancel_flag.store(false, std::memory_order_relaxed);
        const enhancer::UpscaleConfig cfg = map_c_config(*job_cfg);

        auto cb = [&](int pct, const std::string& backend, int tile_sz) {
            if (progress_fn) {
                progress_fn(pct, backend.c_str(), tile_sz, user_data);
            }
        };

        const enhancer::UpscaleResult res = enhancer::run_upscale_pipeline(
            nullptr,
            src_rgba,
            src_w,
            src_h,
            src_stride_bytes,
            nullptr,
            cfg,
            g_cancel_flag,
            cb
        );
        fill_c_result(res, out_result);
    } catch (const std::bad_alloc&) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "std::bad_alloc in enhancer_upscale_rgba");
    } catch (const std::exception& ex) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_EXCEPTION);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), ex.what());
    } catch (...) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_EXCEPTION);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "Unknown native exception");
    }
}

ENHANCER_API void ENHANCER_CALL enhancer_upscale_file(
    const char* input_path_utf8,
    const EnhancerCJobConfig* job_cfg,
    EnhancerProgressFn progress_fn,
    void* user_data,
    int32_t* out_src_w,
    int32_t* out_src_h,
    EnhancerCResult* out_result
) {
    if (!out_result) {
        return;
    }
    std::memset(out_result, 0, sizeof(EnhancerCResult));
    if (out_src_w) *out_src_w = 0;
    if (out_src_h) *out_src_h = 0;

    if (!input_path_utf8 || !job_cfg) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_INVALID_ARGS);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "Null input_path_utf8 or job_cfg");
        return;
    }

    try {
        enhancer::NativeByteBuffer rgba_buf;
        int w = 0;
        int h = 0;
        std::string decode_err;
        if (!enhancer::decode_image_file_utf8_to_rgba(input_path_utf8, rgba_buf, w, h, &decode_err)) {
            out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_INVALID_ARGS);
            copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), decode_err);
            return;
        }
        if (out_src_w) *out_src_w = w;
        if (out_src_h) *out_src_h = h;

        std::lock_guard<std::mutex> lock(g_engine_session_mutex);
        g_cancel_flag.store(false, std::memory_order_relaxed);
        const enhancer::UpscaleConfig cfg = map_c_config(*job_cfg);

        auto cb = [&](int pct, const std::string& backend, int tile_sz) {
            if (progress_fn) {
                progress_fn(pct, backend.c_str(), tile_sz, user_data);
            }
        };

        const enhancer::UpscaleResult res = enhancer::run_upscale_pipeline(
            nullptr,
            rgba_buf.get(),
            w,
            h,
            w * 4,
            [&]() { rgba_buf.reset(); },
            cfg,
            g_cancel_flag,
            cb
        );
        rgba_buf.reset();
        fill_c_result(res, out_result);
    } catch (const std::bad_alloc&) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "std::bad_alloc in enhancer_upscale_file");
    } catch (const std::exception& ex) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_EXCEPTION);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), ex.what());
    } catch (...) {
        out_result->status_code = static_cast<int32_t>(enhancer::EnhancerStatus::ERR_EXCEPTION);
        copy_utf8_trunc(out_result->error_detail, sizeof(out_result->error_detail), "Unknown native exception");
    }
}

ENHANCER_API int32_t ENHANCER_CALL enhancer_run_self_test(
    const char* models_dir_utf8,
    int32_t* out_vulkan_passed,
    int64_t* out_vulkan_ms,
    int32_t* out_cpu_passed,
    int64_t* out_cpu_ms,
    char* out_full_log_utf8,
    int32_t out_log_cap
) {
    try {
        std::lock_guard<std::mutex> lock(g_engine_session_mutex);
        const std::string dir = models_dir_utf8 ? models_dir_utf8 : "";
        const enhancer::SelfTestReport rep = enhancer::run_native_self_test(nullptr, dir);
        if (out_vulkan_passed) *out_vulkan_passed = rep.vulkan.passed ? 1 : 0;
        if (out_vulkan_ms) *out_vulkan_ms = rep.vulkan.elapsed_ms;
        if (out_cpu_passed) *out_cpu_passed = rep.cpu.passed ? 1 : 0;
        if (out_cpu_ms) *out_cpu_ms = rep.cpu.elapsed_ms;
        if (out_full_log_utf8 && out_log_cap > 0) {
            copy_utf8_trunc(out_full_log_utf8, static_cast<size_t>(out_log_cap), rep.full_log);
        }
        return rep.cpu.passed ? 0 : -1;
    } catch (const std::exception& ex) {
        if (out_full_log_utf8 && out_log_cap > 0) {
            copy_utf8_trunc(out_full_log_utf8, static_cast<size_t>(out_log_cap), ex.what());
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

ENHANCER_API int32_t ENHANCER_CALL enhancer_run_seam_test(
    const char* models_dir_utf8,
    EnhancerSeamProgressFn progress_fn,
    void* user_data,
    int32_t* out_overall_pass,
    char* out_summary_utf8,
    int32_t out_summary_cap,
    char* out_table_utf8,
    int32_t out_table_cap
) {
    try {
        std::lock_guard<std::mutex> lock(g_engine_session_mutex);
        g_seam_cancel_flag.store(false, std::memory_order_relaxed);
        const std::string dir = models_dir_utf8 ? models_dir_utf8 : "";

        auto cb = [&](int done, int total, const std::string& label) {
            if (progress_fn) {
                progress_fn(done, total, label.c_str(), user_data);
            }
        };

        const enhancer::SeamTestReport rep = enhancer::run_native_seam_test(
            nullptr,
            g_seam_cancel_flag,
            cb,
            dir
        );
        if (out_overall_pass) *out_overall_pass = rep.overall_pass ? 1 : 0;
        if (out_summary_utf8 && out_summary_cap > 0) {
            copy_utf8_trunc(out_summary_utf8, static_cast<size_t>(out_summary_cap), rep.summary);
        }
        if (out_table_utf8 && out_table_cap > 0) {
            copy_utf8_trunc(out_table_utf8, static_cast<size_t>(out_table_cap), rep.formatted_table);
        }
        return static_cast<int32_t>(rep.status);
    } catch (const std::exception& ex) {
        if (out_summary_utf8 && out_summary_cap > 0) {
            copy_utf8_trunc(out_summary_utf8, static_cast<size_t>(out_summary_cap), ex.what());
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

ENHANCER_API void ENHANCER_CALL enhancer_shutdown(void) {
    enhancer::shutdown_vulkan_runtime();
}

} // extern "C"
