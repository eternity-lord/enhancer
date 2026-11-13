#ifndef ENHANCER_C_API_H
#define ENHANCER_C_API_H

#include <stdint.h>

#if defined(_WIN32)
  #if defined(ENHANCER_ENGINE_EXPORTS)
    #define ENHANCER_API __declspec(dllexport)
  #else
    #define ENHANCER_API __declspec(dllimport)
  #endif
  #define ENHANCER_CALL __cdecl
#else
  #define ENHANCER_API __attribute__((visibility("default")))
  #define ENHANCER_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (ENHANCER_CALL *EnhancerProgressFn)(
    int32_t progress_pct,
    const char* backend_utf8,
    int32_t tile_size,
    void* user_data
);

typedef void (ENHANCER_CALL *EnhancerSeamProgressFn)(
    int32_t completed_cases,
    int32_t total_cases,
    const char* current_label_utf8,
    void* user_data
);

typedef struct EnhancerCJobConfig {
    const char* models_dir_utf8;
    const char* param_rel_utf8;
    const char* bin_rel_utf8;
    const char* output_path_utf8;
    int32_t model_scale;
    int32_t target_long_side;
    int32_t tile_size;
    int32_t disable_fp16;
    int32_t max_quality;
    int32_t tta_x8;
    int32_t safe_mode;
    int32_t force_cpu;
    int32_t gpu_index;
    int32_t num_threads;
    int32_t sharpness;
    int32_t enhance_mode;
    int32_t enhance_faces;
    float face_strength;
    int32_t light_ai;
    float light_ai_strength;
    float denoise_strength;
    float ultra_intensity;
    float ultra_size;
    int32_t dpi;
    int32_t output_format;
    int32_t jpeg_quality;
} EnhancerCJobConfig;

typedef struct EnhancerCResult {
    int32_t status_code;
    int32_t out_width;
    int32_t out_height;
    int32_t used_cpu_fallback;
    int32_t final_tile_size;
    int32_t faces_detected;
    int32_t faces_restored;
    char backend_used[256];
    char fallback_summary[512];
    char error_detail[2048];
} EnhancerCResult;

typedef struct EnhancerCGpuInfo {
    int32_t index;
    uint32_t vendor_id;
    uint32_t type;           // 0 = Discrete, 1 = Integrated, 2 = Virtual, 3 = CPU
    uint32_t heap_budget_mb;
    int32_t is_recommended;
    char name[256];
} EnhancerCGpuInfo;

ENHANCER_API int32_t ENHANCER_CALL enhancer_get_cpu_thread_count(int32_t* out_big_cores);

ENHANCER_API void ENHANCER_CALL enhancer_get_system_memory_mb(
    int32_t* out_avail_mb,
    int32_t* out_total_mb
);

ENHANCER_API int32_t ENHANCER_CALL enhancer_enumerate_gpus(
    EnhancerCGpuInfo* out_gpus,
    int32_t max_gpus
);

ENHANCER_API void ENHANCER_CALL enhancer_get_diagnostics_summary(
    char* out_utf8,
    int32_t out_cap
);

ENHANCER_API int32_t ENHANCER_CALL enhancer_calibrate_fp16_policy(
    const char* models_dir_utf8,
    int32_t* out_vulkan_available,
    int32_t* out_recommend_disable_fp16,
    char* out_reason_utf8,
    int32_t out_reason_cap
);

ENHANCER_API void ENHANCER_CALL enhancer_compute_dimensions(
    int32_t src_w,
    int32_t src_h,
    int32_t model_scale,
    int32_t target_long_side,
    int32_t max_quality,
    int32_t out_plan_9[9]
);

ENHANCER_API void ENHANCER_CALL enhancer_reset_cancel(void);
ENHANCER_API void ENHANCER_CALL enhancer_request_cancel(void);
ENHANCER_API void ENHANCER_CALL enhancer_cancel_seam_test(void);

ENHANCER_API void ENHANCER_CALL enhancer_upscale_rgba(
    const uint8_t* src_rgba,
    int32_t src_w,
    int32_t src_h,
    int32_t src_stride_bytes,
    const EnhancerCJobConfig* job_cfg,
    EnhancerProgressFn progress_fn,
    void* user_data,
    EnhancerCResult* out_result
);

ENHANCER_API void ENHANCER_CALL enhancer_upscale_file(
    const char* input_path_utf8,
    const EnhancerCJobConfig* job_cfg,
    EnhancerProgressFn progress_fn,
    void* user_data,
    int32_t* out_src_w,
    int32_t* out_src_h,
    EnhancerCResult* out_result
);

ENHANCER_API int32_t ENHANCER_CALL enhancer_run_self_test(
    const char* models_dir_utf8,
    int32_t* out_vulkan_passed,
    int64_t* out_vulkan_ms,
    int32_t* out_cpu_passed,
    int64_t* out_cpu_ms,
    char* out_full_log_utf8,
    int32_t out_log_cap
);

ENHANCER_API int32_t ENHANCER_CALL enhancer_run_seam_test(
    const char* models_dir_utf8,
    EnhancerSeamProgressFn progress_fn,
    void* user_data,
    int32_t* out_overall_pass,
    char* out_summary_utf8,
    int32_t out_summary_cap,
    char* out_table_utf8,
    int32_t out_table_cap
);

ENHANCER_API void ENHANCER_CALL enhancer_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // ENHANCER_C_API_H
