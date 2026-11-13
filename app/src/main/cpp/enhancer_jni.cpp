#include "enhancer_types.h"
#include "image_io.h"
#include "resizer.h"
#include "upscaler.h"

#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <vector>

#define LOG_TAG "EnhancerJNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_cancel_requested{false};
std::atomic<bool> g_seam_test_cancel_requested{false};

std::string jstring_to_std(JNIEnv* env, jstring jstr) {
    if (!jstr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    if (!chars) {
        return {};
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return result;
}

class ScopedBitmapPixels {
public:
    ScopedBitmapPixels(JNIEnv* env, jobject bitmap) : env_(env), bitmap_(bitmap) {
        if (!env_ || !bitmap_) {
            fail_reason_ = "JNIEnv or Bitmap is null";
            return;
        }
        const int info_rc = AndroidBitmap_getInfo(env_, bitmap_, &info_);
        if (info_rc != ANDROID_BITMAP_RESULT_SUCCESS) {
            fail_reason_ = "AndroidBitmap_getInfo rc=" + std::to_string(info_rc);
            return;
        }
        if (info_.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
            fail_reason_ = "Bitmap format=" + std::to_string(info_.format) + " (expected RGBA_8888=1)";
            return;
        }
        const int lock_rc = AndroidBitmap_lockPixels(env_, bitmap_, &pixels_);
        if (lock_rc != ANDROID_BITMAP_RESULT_SUCCESS) {
            fail_reason_ = "AndroidBitmap_lockPixels rc=" + std::to_string(lock_rc);
            pixels_ = nullptr;
        }
    }

    ~ScopedBitmapPixels() {
        unlock();
    }

    void unlock() noexcept {
        if (pixels_ && env_ && bitmap_) {
            AndroidBitmap_unlockPixels(env_, bitmap_);
            pixels_ = nullptr;
        }
    }

    // Unlocks native pixel pin AND immediately recycles the temporary decoded Java Bitmap
    // so Android frees the source RGBA heap BEFORE final output buffer / encoding runs.
    void unlock_and_recycle() noexcept {
        unlock();
        if (!recycled_ && env_ && bitmap_) {
            recycled_ = true;
            jclass bmp_cls = env_->GetObjectClass(bitmap_);
            if (bmp_cls) {
                jmethodID recycle_mid = env_->GetMethodID(bmp_cls, "recycle", "()V");
                if (recycle_mid) {
                    env_->CallVoidMethod(bitmap_, recycle_mid);
                }
                env_->DeleteLocalRef(bmp_cls);
                if (env_->ExceptionCheck()) {
                    env_->ExceptionClear();
                }
            }
        }
    }

    bool valid() const noexcept { return pixels_ != nullptr; }
    uint8_t* mutable_data() const noexcept { return static_cast<uint8_t*>(pixels_); }
    const uint8_t* data() const noexcept { return static_cast<const uint8_t*>(pixels_); }
    int width() const noexcept { return static_cast<int>(info_.width); }
    int height() const noexcept { return static_cast<int>(info_.height); }
    int stride() const noexcept { return static_cast<int>(info_.stride); }
    const std::string& fail_reason() const noexcept { return fail_reason_; }

private:
    JNIEnv* env_ = nullptr;
    jobject bitmap_ = nullptr;
    AndroidBitmapInfo info_{};
    void* pixels_ = nullptr;
    bool recycled_ = false;
    std::string fail_reason_;
};

jobject build_native_result(JNIEnv* env, const enhancer::UpscaleResult& res) {
    jclass cls = env->FindClass("com/enhancer/app/nativebridge/NativeResult");
    if (!cls) {
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(
        cls,
        "<init>",
        "(IIILjava/lang/String;ZILjava/lang/String;Ljava/lang/String;II)V"
    );
    if (!ctor) {
        env->DeleteLocalRef(cls);
        return nullptr;
    }

    jstring j_backend = env->NewStringUTF(res.backend_used.c_str());
    jstring j_fallback = env->NewStringUTF(res.fallback_summary.c_str());
    jstring j_error = env->NewStringUTF(res.error_detail.c_str());

    jobject obj = env->NewObject(
        cls,
        ctor,
        static_cast<jint>(res.status),
        static_cast<jint>(res.out_width),
        static_cast<jint>(res.out_height),
        j_backend,
        static_cast<jboolean>(res.used_cpu_fallback ? JNI_TRUE : JNI_FALSE),
        static_cast<jint>(res.final_tile_size),
        j_fallback,
        j_error,
        static_cast<jint>(res.faces_detected),
        static_cast<jint>(res.faces_restored)
    );

    if (j_backend) {
        env->DeleteLocalRef(j_backend);
    }
    if (j_fallback) {
        env->DeleteLocalRef(j_fallback);
    }
    if (j_error) {
        env->DeleteLocalRef(j_error);
    }
    env->DeleteLocalRef(cls);
    return obj;
}

} // namespace

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    enhancer::shutdown_vulkan_runtime();
}

extern "C" JNIEXPORT void JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeResetCancel(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    g_cancel_requested.store(false, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeCancel(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    g_cancel_requested.store(true, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeCancelSeamTest(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    g_seam_test_cancel_requested.store(true, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeShouldDefaultDisableFp16(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    return enhancer::should_default_disable_fp16_sync() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeCalibrateFp16Policy(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject asset_manager_obj
) {
    jclass string_cls = env->FindClass("java/lang/String");
    if (!string_cls) {
        return nullptr;
    }

    auto make_four = [&](const std::string items[4]) -> jobjectArray {
        jstring empty = env->NewStringUTF("");
        jobjectArray arr = env->NewObjectArray(4, string_cls, empty);
        if (empty) {
            env->DeleteLocalRef(empty);
        }
        if (!arr) {
            return nullptr;
        }
        for (int i = 0; i < 4; ++i) {
            jstring s = env->NewStringUTF(items[i].c_str());
            if (s) {
                env->SetObjectArrayElement(arr, i, s);
                env->DeleteLocalRef(s);
            }
        }
        return arr;
    };

    try {
        AAssetManager* asset_mgr = asset_manager_obj
            ? AAssetManager_fromJava(env, asset_manager_obj)
            : nullptr;
        const enhancer::Fp16CalibrationReport rep =
            enhancer::calibrate_vulkan_fp16_policy(asset_mgr);
        const std::string fields[4] = {
            rep.recommend_disable_fp16 ? "1" : "0",
            rep.vulkan_available ? "1" : "0",
            rep.is_adreno ? "1" : "0",
            rep.reason
        };
        jobjectArray out = make_four(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (...) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        const bool def_off = enhancer::should_default_disable_fp16_sync();
        const std::string fields[4] = {
            def_off ? "1" : "0",
            "0",
            def_off ? "1" : "0",
            def_off ? "FP16 default: OFF (hardware policy)" : "FP16 default: ON"
        };
        jobjectArray out = make_four(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    }
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeRunSeamTest(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject asset_manager_obj,
    jobject progress_listener
) {
    g_seam_test_cancel_requested.store(false, std::memory_order_relaxed);
    jclass string_cls = env->FindClass("java/lang/String");
    if (!string_cls) {
        return nullptr;
    }

    auto make_vec_array = [&](const std::vector<std::string>& items) -> jobjectArray {
        jstring empty = env->NewStringUTF("");
        jobjectArray arr = env->NewObjectArray(static_cast<jsize>(items.size()), string_cls, empty);
        if (empty) {
            env->DeleteLocalRef(empty);
        }
        if (!arr) {
            return nullptr;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            jstring s = env->NewStringUTF(items[i].c_str());
            if (s) {
                env->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
                env->DeleteLocalRef(s);
            }
        }
        return arr;
    };

    try {
        AAssetManager* asset_mgr = AAssetManager_fromJava(env, asset_manager_obj);
        if (!asset_mgr) {
            const std::vector<std::string> err = {
                std::to_string(static_cast<int>(enhancer::EnhancerStatus::ERR_ASSET_LOAD)),
                "FAIL",
                "AssetManager is null",
                "",
                "Seam test failed: AssetManager is null"
            };
            jobjectArray out = make_vec_array(err);
            env->DeleteLocalRef(string_cls);
            return out;
        }

        jmethodID progress_mid = nullptr;
        if (progress_listener != nullptr) {
            jclass listener_cls = env->GetObjectClass(progress_listener);
            if (listener_cls) {
                progress_mid = env->GetMethodID(
                    listener_cls,
                    "onProgress",
                    "(ILjava/lang/String;I)V"
                );
                env->DeleteLocalRef(listener_cls);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    progress_mid = nullptr;
                }
            }
        }

        auto prog_cb = [&](int done_cases, int total_cases, const std::string& label) {
            if (!progress_listener || !progress_mid) {
                return;
            }
            jstring j_label = env->NewStringUTF(label.c_str());
            env->CallVoidMethod(
                progress_listener,
                progress_mid,
                static_cast<jint>(done_cases),
                j_label,
                static_cast<jint>(total_cases)
            );
            if (j_label) {
                env->DeleteLocalRef(j_label);
            }
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                g_seam_test_cancel_requested.store(true, std::memory_order_relaxed);
            }
        };

        const enhancer::SeamTestReport rep = enhancer::run_native_seam_test(
            asset_mgr,
            g_seam_test_cancel_requested,
            prog_cb
        );

        std::vector<std::string> fields;
        fields.reserve(5u + rep.rows.size());
        fields.push_back(std::to_string(static_cast<int>(rep.status)));
        fields.push_back(rep.overall_pass ? "PASS" : "FAIL");
        fields.push_back(rep.summary);
        fields.push_back(rep.fp16_calibration.reason);
        fields.push_back(rep.formatted_table);

        for (const enhancer::SeamTestCaseRow& r : rep.rows) {
            char row_buf[320];
            const char* status_str = !r.backend_available ? "N/A" : (r.passed ? "PASS" : "FAIL");
            std::snprintf(
                row_buf,
                sizeof(row_buf),
                "%s\t%s\t%s\t%d\t%s\t%.2f/%.2f\t%d/%d\t%.2f\t%lld\t%s",
                r.backend.c_str(),
                r.fp16_enabled ? "ON" : "OFF",
                r.max_quality ? "ON" : "OFF",
                r.tile_size,
                status_str,
                static_cast<double>(r.mean_boundary_jump),
                static_cast<double>(r.mean_interior_jump),
                r.max_boundary_jump,
                r.max_interior_jump,
                static_cast<double>(r.neighbor_mean_diff),
                static_cast<long long>(r.elapsed_ms),
                r.note.c_str()
            );
            fields.emplace_back(row_buf);
        }

        jobjectArray out = make_vec_array(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (const std::bad_alloc&) {
        const std::vector<std::string> err = {
            std::to_string(static_cast<int>(enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY)),
            "FAIL",
            "Out of memory during Seam Test",
            "",
            "Seam test failed: std::bad_alloc"
        };
        jobjectArray out = make_vec_array(err);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (const std::exception& ex) {
        const std::vector<std::string> err = {
            std::to_string(static_cast<int>(enhancer::EnhancerStatus::ERR_EXCEPTION)),
            "FAIL",
            std::string("Exception: ") + ex.what(),
            "",
            std::string("Seam test exception: ") + ex.what()
        };
        jobjectArray out = make_vec_array(err);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (...) {
        const std::vector<std::string> err = {
            std::to_string(static_cast<int>(enhancer::EnhancerStatus::ERR_EXCEPTION)),
            "FAIL",
            "Unknown native exception during Seam Test",
            "",
            "Seam test failed: unknown native exception"
        };
        jobjectArray out = make_vec_array(err);
        env->DeleteLocalRef(string_cls);
        return out;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeQueryGpuInfo(
    JNIEnv* env,
    jobject /*thiz*/
) {
    try {
        const std::string info = enhancer::query_vulkan_device_info();
        return env->NewStringUTF(info.c_str());
    } catch (...) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return env->NewStringUTF("CPU only");
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeQueryGpuDiagnostics(
    JNIEnv* env,
    jobject /*thiz*/
) {
    try {
        const std::string diag = enhancer::query_vulkan_diagnostics_summary();
        return env->NewStringUTF(diag.c_str());
    } catch (...) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return env->NewStringUTF("Vulkan diagnostics unavailable");
    }
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeRunSelfTest(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject asset_manager_obj
) {
    jclass string_cls = env->FindClass("java/lang/String");
    if (!string_cls) {
        return nullptr;
    }

    auto make_array = [&](const std::string items[7]) -> jobjectArray {
        jstring empty = env->NewStringUTF("");
        jobjectArray arr = env->NewObjectArray(7, string_cls, empty);
        if (empty) {
            env->DeleteLocalRef(empty);
        }
        if (!arr) {
            return nullptr;
        }
        for (int i = 0; i < 7; ++i) {
            jstring s = env->NewStringUTF(items[i].c_str());
            if (s) {
                env->SetObjectArrayElement(arr, i, s);
                env->DeleteLocalRef(s);
            }
        }
        return arr;
    };

    try {
        AAssetManager* asset_mgr = AAssetManager_fromJava(env, asset_manager_obj);
        if (!asset_mgr) {
            const std::string err = enhancer::format_step_diagnostic(
                enhancer::EnhancerStatus::ERR_ASSET_LOAD,
                "AAssetManager_fromJava",
                0,
                64,
                "SelfTest",
                0,
                "AAssetManager is null"
            );
            const std::string fields[7] = {"FAIL", "0", err, "FAIL", "0", err, err};
            jobjectArray out = make_array(fields);
            env->DeleteLocalRef(string_cls);
            return out;
        }

        const enhancer::SelfTestReport rep = enhancer::run_native_self_test(asset_mgr);
        const std::string fields[7] = {
            rep.vulkan.passed ? "PASS" : "FAIL",
            std::to_string(rep.vulkan.elapsed_ms),
            rep.vulkan.detail,
            rep.cpu.passed ? "PASS" : "FAIL",
            std::to_string(rep.cpu.elapsed_ms),
            rep.cpu.detail,
            rep.full_log
        };
        jobjectArray out = make_array(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (const std::bad_alloc&) {
        const std::string err = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY,
            "std::bad_alloc(nativeRunSelfTest)",
            0,
            64,
            "SelfTest",
            0,
            ""
        );
        const std::string fields[7] = {"FAIL", "0", err, "FAIL", "0", err, err};
        jobjectArray out = make_array(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (const std::exception& ex) {
        const std::string err = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_EXCEPTION,
            "std::exception(nativeRunSelfTest)",
            0,
            64,
            "SelfTest",
            0,
            ex.what()
        );
        const std::string fields[7] = {"FAIL", "0", err, "FAIL", "0", err, err};
        jobjectArray out = make_array(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    } catch (...) {
        const std::string err = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_EXCEPTION,
            "unknown_exception(nativeRunSelfTest)",
            0,
            64,
            "SelfTest",
            0,
            ""
        );
        const std::string fields[7] = {"FAIL", "0", err, "FAIL", "0", err, err};
        jobjectArray out = make_array(fields);
        env->DeleteLocalRef(string_cls);
        return out;
    }
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeComputeDimensions(
    JNIEnv* env,
    jobject /*thiz*/,
    jint src_w,
    jint src_h,
    jint model_scale,
    jint target_long_side,
    jboolean max_quality
) {
    const enhancer::DimensionPlan plan = enhancer::compute_dimensions(
        static_cast<int>(src_w),
        static_cast<int>(src_h),
        static_cast<int>(model_scale),
        static_cast<int>(target_long_side),
        max_quality == JNI_TRUE
    );
    jint values[9] = {
        plan.work_w,
        plan.work_h,
        plan.model_out_w,
        plan.model_out_h,
        plan.final_w,
        plan.final_h,
        plan.pre_downscaled ? 1 : 0,
        plan.post_upscaled ? 1 : 0,
        plan.post_downscaled ? 1 : 0
    };
    jintArray arr = env->NewIntArray(9);
    if (arr) {
        env->SetIntArrayRegion(arr, 0, 9, values);
    }
    return arr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeResizeBitmapBicubic(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject src_bitmap,
    jdouble roi_x0,
    jdouble roi_y0,
    jdouble roi_x1,
    jdouble roi_y1,
    jobject dst_bitmap
) {
    try {
        ScopedBitmapPixels src_lock(env, src_bitmap);
        ScopedBitmapPixels dst_lock(env, dst_bitmap);
        if (!src_lock.valid() || !dst_lock.valid()) {
            return JNI_FALSE;
        }
        const bool ok = enhancer::resize_rgba_roi_bicubic(
            src_lock.data(),
            src_lock.width(),
            src_lock.height(),
            src_lock.stride(),
            static_cast<double>(roi_x0),
            static_cast<double>(roi_y0),
            static_cast<double>(roi_x1),
            static_cast<double>(roi_y1),
            dst_lock.mutable_data(),
            dst_lock.width(),
            dst_lock.height(),
            dst_lock.stride()
        );
        return ok ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeUpscaleImage(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject asset_manager_obj,
    jobject src_bitmap,
    jstring param_asset_path,
    jstring bin_asset_path,
    jint model_scale,
    jint target_long_side,
    jint tile_size,
    jboolean disable_fp16,
    jboolean max_quality,
    jboolean tta_x8,
    jboolean safe_mode,
    jboolean force_cpu,
    jint sharpness,
    jint enhance_mode,
    jboolean enhance_faces,
    jfloat face_strength,
    jboolean light_ai,
    jfloat light_ai_strength,
    jfloat denoise_strength,
    jfloat ultra_intensity,
    jfloat ultra_size,
    jint dpi,
    jint output_format,
    jint jpeg_quality,
    jint output_fd,
    jobject progress_listener
) {
    g_cancel_requested.store(false, std::memory_order_relaxed);
    enhancer::UpscaleResult res;
    const int req_tile = (safe_mode == JNI_TRUE) ? 64 : (tile_size > 0 ? static_cast<int>(tile_size) : 200);
    const std::string req_backend = (safe_mode == JNI_TRUE) ? "CPU (Safe mode)" : "Vulkan/CPU";

    try {
        AAssetManager* asset_mgr = AAssetManager_fromJava(env, asset_manager_obj);
        if (!asset_mgr) {
            enhancer::UniqueFd close_guard(static_cast<int>(output_fd));
            res.status = enhancer::EnhancerStatus::ERR_ASSET_LOAD;
            res.error_detail = enhancer::format_step_diagnostic(
                enhancer::EnhancerStatus::ERR_ASSET_LOAD,
                "AAssetManager_fromJava",
                0,
                req_tile,
                req_backend,
                0,
                "AAssetManager is null"
            );
            return build_native_result(env, res);
        }

        ScopedBitmapPixels locked_bitmap(env, src_bitmap);
        if (!locked_bitmap.valid()) {
            enhancer::UniqueFd close_guard(static_cast<int>(output_fd));
            res.status = enhancer::EnhancerStatus::ERR_BITMAP_LOCK;
            res.error_detail = enhancer::format_step_diagnostic(
                enhancer::EnhancerStatus::ERR_BITMAP_LOCK,
                "AndroidBitmap_lockPixels",
                0,
                req_tile,
                req_backend,
                0,
                locked_bitmap.fail_reason()
            );
            return build_native_result(env, res);
        }

        jmethodID progress_mid = nullptr;
        if (progress_listener != nullptr) {
            jclass listener_cls = env->GetObjectClass(progress_listener);
            if (listener_cls) {
                progress_mid = env->GetMethodID(
                    listener_cls,
                    "onProgress",
                    "(ILjava/lang/String;I)V"
                );
                env->DeleteLocalRef(listener_cls);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    progress_mid = nullptr;
                }
            }
        }

        enhancer::UpscaleConfig cfg;
        cfg.param_asset_path = jstring_to_std(env, param_asset_path);
        cfg.bin_asset_path = jstring_to_std(env, bin_asset_path);
        cfg.model_scale = static_cast<int>(model_scale);
        cfg.target_long_side = static_cast<int>(target_long_side);
        cfg.requested_tile_size = static_cast<int>(tile_size);
        cfg.disable_fp16 = (disable_fp16 == JNI_TRUE);
        cfg.max_quality = (max_quality == JNI_TRUE);
        cfg.tta_x8 = (tta_x8 == JNI_TRUE);
        cfg.safe_mode = (safe_mode == JNI_TRUE);
        cfg.force_cpu = (force_cpu == JNI_TRUE);
        cfg.sharpness = static_cast<int>(sharpness);
        const int clamped_mode = (enhance_mode >= 0 && enhance_mode <= 6)
            ? static_cast<int>(enhance_mode)
            : static_cast<int>(enhancer::EnhanceMode::BALANCED);
        cfg.mode = static_cast<enhancer::EnhanceMode>(clamped_mode);
        cfg.enhance_faces = (enhance_faces == JNI_TRUE);
        cfg.face_strength = static_cast<float>(face_strength);
        cfg.light_ai = (light_ai == JNI_TRUE);
        cfg.light_ai_strength = static_cast<float>(light_ai_strength);
        cfg.denoise_strength = static_cast<float>(denoise_strength);
        cfg.ultra_intensity = static_cast<float>(ultra_intensity);
        cfg.ultra_size = static_cast<float>(ultra_size);
        cfg.dpi = (dpi > 0) ? static_cast<int>(dpi) : 300;
        cfg.format = (output_format == 1)
            ? enhancer::ImageFormat::PNG
            : enhancer::ImageFormat::JPEG;
        cfg.jpeg_quality = static_cast<int>(jpeg_quality);
        cfg.output_fd = static_cast<int>(output_fd);

        auto cb = [&](int pct, const std::string& backend, int active_tile) {
            if (!progress_listener || !progress_mid) {
                return;
            }
            jstring j_backend = env->NewStringUTF(backend.c_str());
            env->CallVoidMethod(
                progress_listener,
                progress_mid,
                static_cast<jint>(pct),
                j_backend,
                static_cast<jint>(active_tile)
            );
            if (j_backend) {
                env->DeleteLocalRef(j_backend);
            }
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                g_cancel_requested.store(true, std::memory_order_relaxed);
            }
        };

        res = enhancer::run_upscale_pipeline(
            asset_mgr,
            locked_bitmap.data(),
            locked_bitmap.width(),
            locked_bitmap.height(),
            locked_bitmap.stride(),
            [&]() { locked_bitmap.unlock_and_recycle(); },
            cfg,
            g_cancel_requested,
            cb
        );
        locked_bitmap.unlock_and_recycle();
        return build_native_result(env, res);
    } catch (const std::bad_alloc&) {
        LOGE("Caught std::bad_alloc at JNI boundary");
        res.status = enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY;
        res.error_detail = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY,
            "std::bad_alloc(JNI_boundary)",
            0,
            req_tile,
            req_backend,
            0,
            ""
        );
        return build_native_result(env, res);
    } catch (const std::exception& ex) {
        LOGE("Caught std::exception at JNI boundary: %s", ex.what());
        res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        res.error_detail = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_EXCEPTION,
            "std::exception(JNI_boundary)",
            0,
            req_tile,
            req_backend,
            0,
            ex.what()
        );
        return build_native_result(env, res);
    } catch (...) {
        LOGE("Caught unknown exception at JNI boundary");
        res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        res.error_detail = enhancer::format_step_diagnostic(
            enhancer::EnhancerStatus::ERR_EXCEPTION,
            "unknown_exception(JNI_boundary)",
            0,
            req_tile,
            req_backend,
            0,
            ""
        );
        return build_native_result(env, res);
    }
}
