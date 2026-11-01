#include "enhancer_types.h"
#include "image_io.h"
#include "upscaler.h"

#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <exception>
#include <new>
#include <string>

#define JNI_LOG_TAG "EnhancerJNI"
#define JNI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, JNI_LOG_TAG, __VA_ARGS__)

namespace {

std::atomic<bool> g_cancel_flag{false};

std::string jstring_to_utf8(JNIEnv* env, jstring jstr) {
    if (!env || !jstr) {
        return "";
    }
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    if (!chars) {
        return "";
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return out;
}

void notify_finished_metadata(
    JNIEnv* env,
    jobject callback,
    jmethodID on_finished_mid,
    const enhancer::UpscaleResult& result
) {
    if (!env || !callback || !on_finished_mid) {
        return;
    }
    jstring j_backend = env->NewStringUTF(result.backend_used.c_str());
    jstring j_error = env->NewStringUTF(result.error_detail.c_str());
    if (j_backend && j_error) {
        env->CallVoidMethod(
            callback,
            on_finished_mid,
            static_cast<jint>(result.out_width),
            static_cast<jint>(result.out_height),
            j_backend,
            static_cast<jboolean>(result.used_cpu_fallback ? JNI_TRUE : JNI_FALSE),
            static_cast<jint>(result.final_tile_size),
            j_error
        );
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    if (j_backend) {
        env->DeleteLocalRef(j_backend);
    }
    if (j_error) {
        env->DeleteLocalRef(j_error);
    }
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    try {
        enhancer::shutdown_vulkan_instance();
    } catch (...) {
        // Never allow exceptions out of JNI_OnUnload
    }
}

JNIEXPORT jstring JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeGetVulkanInfo(
    JNIEnv* env,
    jobject /*thiz*/
) {
    try {
        const std::string info = enhancer::query_vulkan_device_info();
        return env->NewStringUTF(info.c_str());
    } catch (const std::exception& e) {
        JNI_LOGE("Exception in nativeGetVulkanInfo: %s", e.what());
        return env->NewStringUTF("");
    } catch (...) {
        JNI_LOGE("Unknown exception in nativeGetVulkanInfo");
        return env->NewStringUTF("");
    }
}

JNIEXPORT void JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeResetCancel(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    g_cancel_flag.store(false, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeCancel(
    JNIEnv* /*env*/,
    jobject /*thiz*/
) {
    g_cancel_flag.store(true, std::memory_order_relaxed);
}

JNIEXPORT jint JNICALL
Java_com_enhancer_app_nativebridge_NativeEnhancer_nativeProcessBitmap(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject asset_manager,
    jobject source_bitmap,
    jstring param_asset_path,
    jstring bin_asset_path,
    jint model_scale,
    jint target_long_side,
    jint tile_size,
    jboolean disable_fp16,
    jint output_format,
    jint jpeg_quality,
    jint output_fd,
    jobject callback
) {
    // Take immediate ownership of the detached ParcelFileDescriptor fd so it never leaks.
    enhancer::UniqueFd fd_guard(static_cast<int>(output_fd));

    bool bitmap_locked = false;
    auto unlock_bitmap = [&]() noexcept {
        if (bitmap_locked && env && source_bitmap) {
            AndroidBitmap_unlockPixels(env, source_bitmap);
            bitmap_locked = false;
        }
    };

    jmethodID on_progress_mid = nullptr;
    jmethodID on_finished_mid = nullptr;
    if (callback != nullptr) {
        jclass cb_cls = env->GetObjectClass(callback);
        if (cb_cls != nullptr) {
            on_progress_mid = env->GetMethodID(cb_cls, "onProgress", "(ILjava/lang/String;I)V");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                on_progress_mid = nullptr;
            }
            on_finished_mid = env->GetMethodID(
                cb_cls,
                "onFinishedMetadata",
                "(IILjava/lang/String;ZILjava/lang/String;)V"
            );
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                on_finished_mid = nullptr;
            }
            env->DeleteLocalRef(cb_cls);
        }
    }

    try {
        if (!asset_manager || !source_bitmap || !param_asset_path || !bin_asset_path || !fd_guard.valid()) {
            enhancer::UpscaleResult err_res;
            err_res.status = enhancer::EnhancerStatus::ERR_INVALID_ARGS;
            err_res.error_detail = "Null JNI argument or invalid output fd";
            notify_finished_metadata(env, callback, on_finished_mid, err_res);
            return static_cast<jint>(err_res.status);
        }

        AAssetManager* mgr = AAssetManager_fromJava(env, asset_manager);
        if (!mgr) {
            enhancer::UpscaleResult err_res;
            err_res.status = enhancer::EnhancerStatus::ERR_ASSET_LOAD;
            err_res.error_detail = "AAssetManager_fromJava returned null";
            notify_finished_metadata(env, callback, on_finished_mid, err_res);
            return static_cast<jint>(err_res.status);
        }

        AndroidBitmapInfo bmp_info{};
        if (AndroidBitmap_getInfo(env, source_bitmap, &bmp_info) != ANDROID_BITMAP_RESULT_SUCCESS) {
            enhancer::UpscaleResult err_res;
            err_res.status = enhancer::EnhancerStatus::ERR_BITMAP_LOCK;
            err_res.error_detail = "AndroidBitmap_getInfo failed";
            notify_finished_metadata(env, callback, on_finished_mid, err_res);
            return static_cast<jint>(err_res.status);
        }

        if (bmp_info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
            bmp_info.width == 0 ||
            bmp_info.height == 0) {
            enhancer::UpscaleResult err_res;
            err_res.status = enhancer::EnhancerStatus::ERR_BITMAP_LOCK;
            err_res.error_detail = "Source Bitmap must be non-empty RGBA_8888";
            notify_finished_metadata(env, callback, on_finished_mid, err_res);
            return static_cast<jint>(err_res.status);
        }

        void* pixels_ptr = nullptr;
        if (AndroidBitmap_lockPixels(env, source_bitmap, &pixels_ptr) != ANDROID_BITMAP_RESULT_SUCCESS ||
            pixels_ptr == nullptr) {
            enhancer::UpscaleResult err_res;
            err_res.status = enhancer::EnhancerStatus::ERR_BITMAP_LOCK;
            err_res.error_detail = "AndroidBitmap_lockPixels failed";
            notify_finished_metadata(env, callback, on_finished_mid, err_res);
            return static_cast<jint>(err_res.status);
        }
        bitmap_locked = true;

        enhancer::UpscaleConfig cfg;
        cfg.param_asset_path = jstring_to_utf8(env, param_asset_path);
        cfg.bin_asset_path = jstring_to_utf8(env, bin_asset_path);
        cfg.model_scale = static_cast<int>(model_scale);
        cfg.target_long_side = static_cast<int>(target_long_side);
        cfg.requested_tile_size = static_cast<int>(tile_size);
        cfg.disable_fp16 = (disable_fp16 == JNI_TRUE);
        cfg.format = (output_format == 1) ? enhancer::ImageFormat::PNG : enhancer::ImageFormat::JPEG;
        cfg.jpeg_quality = static_cast<int>(jpeg_quality);
        cfg.output_fd = fd_guard.get();

        auto progress_fn = [&](int progress, const std::string& backend, int active_tile) {
            if (!callback || !on_progress_mid) {
                return;
            }
            jstring j_backend = env->NewStringUTF(backend.c_str());
            if (!j_backend) {
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
                return;
            }
            env->CallVoidMethod(
                callback,
                on_progress_mid,
                static_cast<jint>(progress),
                j_backend,
                static_cast<jint>(active_tile)
            );
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(j_backend);
        };

        enhancer::UpscaleResult result = enhancer::run_upscale_pipeline(
            mgr,
             static_cast<const uint8_t*>(pixels_ptr),
            static_cast<int>(bmp_info.width),
            static_cast<int>(bmp_info.height),
            static_cast<int>(bmp_info.stride),
            unlock_bitmap,
            cfg,
            std::move(fd_guard),
            g_cancel_flag,
            progress_fn
        );

        unlock_bitmap();
        notify_finished_metadata(env, callback, on_finished_mid, result);
        return static_cast<jint>(result.status);

    } catch (const std::bad_alloc& e) {
        unlock_bitmap();
        fd_guard.close_now();
        JNI_LOGE("Out of memory in nativeProcessBitmap: %s", e.what());
        enhancer::UpscaleResult err_res;
        err_res.status = enhancer::EnhancerStatus::ERR_OUT_OF_MEMORY;
        err_res.error_detail = "Native std::bad_alloc out of memory";
        notify_finished_metadata(env, callback, on_finished_mid, err_res);
        return static_cast<jint>(err_res.status);
    } catch (const std::exception& e) {
        unlock_bitmap();
        fd_guard.close_now();
        JNI_LOGE("Exception in nativeProcessBitmap: %s", e.what());
        enhancer::UpscaleResult err_res;
        err_res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        err_res.error_detail = std::string("Native exception: ") + e.what();
        notify_finished_metadata(env, callback, on_finished_mid, err_res);
        return static_cast<jint>(err_res.status);
    } catch (...) {
        unlock_bitmap();
        fd_guard.close_now();
        JNI_LOGE("Unknown exception in nativeProcessBitmap");
        enhancer::UpscaleResult err_res;
        err_res.status = enhancer::EnhancerStatus::ERR_EXCEPTION;
        err_res.error_detail = "Unknown native exception";
        notify_finished_metadata(env, callback, on_finished_mid, err_res);
        return static_cast<jint>(err_res.status);
    }
}

} // extern "C"
