#include "enhancer_types.h"
#include "image_io.h"
#include "resizer.h"
#include "upscaler.h"

#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <exception>
#include <string>

#include "gpu.h"

#define LOG_TAG "EnhancerJNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_cancel_requested{false};

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
            return;
        }
        if (AndroidBitmap_getInfo(env_, bitmap_, &info_) != ANDROID_BITMAP_RESULT_SUCCESS) {
            return;
        }
        if (info_.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
            return;
        }
        if (AndroidBitmap_lockPixels(env_, bitmap_, &pixels_) != ANDROID_BITMAP_RESULT_SUCCESS) {
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

    bool valid() const noexcept { return pixels_ != nullptr; }
    uint8_t* mutable_data() const noexcept { return static_cast<uint8_t*>(pixels_); }
    const uint8_t* data() const noexcept { return static_cast<const uint8_t*>(pixels_); }
    int width() const noexcept { return static_cast<int>(info_.width); }
    int height() const noexcept { return static_cast<int>(info_.height); }
    int stride() const noexcept { return static_cast<int>(info_.stride); }

private:
    JNIEnv* env_ = nullptr;
    jobject bitmap_ = nullptr;
    AndroidBitmapInfo info_{};
    void* pixels_ = nullptr;
};

jobject build_native_result(JNIEnv* env, const enhancer::UpscaleResult& res) {
    jclass cls = env->FindClass("com/enhancer/app/nativebridge/NativeResult");
    if (!cls) {
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(
        cls,
        "<init>",
        "(IIILjava/lang/String;ZILjava/lang/String;)V"
    );
    if (!ctor) {
        env->DeleteLocalRef(cls);
        return nullptr;
    }

    jstring j_backend = env->NewStringUTF(res.backend_used.c_str());
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
        j_error
    );

    if (j_backend) {
        env->DeleteLocalRef(j_backend);
    }
    if (j_error) {
        env->DeleteLocalRef(j_error);
    }
    env->DeleteLocalRef(cls);
    return obj;
}

} // namespace

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
    jint sharpness,
    jint output_format,
    jint jpeg_quality,
    jint output_fd,
    jobject progress_listener
) {
    g_cancel_requested.store(false, std::memory_order_relaxed);
    enhancer::UpscaleResult res;

    try {
        AAssetManager* asset_mgr = AAssetManager_fromJava(env, asset_manager_obj);
        if (!asset_mgr) {
            enhancer::UniqueFd close_guard(static_cast<int>(output_fd));
            res.status = enhancer::EnhancerStatus::ERR_INVALID_ARGS;
            res.error_detail = "AAssetManager is null";
            return build_native_result(env, res);
        }

        ScopedBitmapPixels locked_bitmap(env, src_bitmap);
        if (!locked_bitmap.valid()) {
            enhancer::UniqueFd close_guard(static_cast<int>(output_fd));
            res.status = enhancer::EnhancerStatus::ERR_BITMAP_LOCK;
            res.error_detail = "Source Bitmap must be valid ARGB_8888";
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
        cfg.sharpness = static_cast<int>(sharpness);
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
            [&]() { locked_bitmap.unlock(); },
            cfg,
            g_cancel_requested,
            cb
        );
        locked_bitmap.unlock();
        return build_native_result(env, res);
    } catch (const std::bad_alloc&) {
        LOGE("Caught std::bad_alloc at JNI boundary");
        res.status = enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY;
        res.error_detail = "Native out of memory (std::bad_alloc)";
        return build_native_result(env, res);
    } catch (const std::exception& ex) {
        LOGE("Caught std::exception at JNI boundary: %s", ex.what());
        res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        res.error_detail = std::string("Native exception: ") + ex.what();
        return build_native_result(env, res);
    } catch (...) {
        LOGE("Caught unknown exception at JNI boundary");
        res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        res.error_detail = "Unknown native exception";
        return build_native_result(env, res);
    }
}
