package com.enhancer.app.nativebridge

import android.content.res.AssetManager
import android.graphics.Bitmap

/**
 * Progress listener invoked from JNI (`onProgress(ILjava/lang/String;I)V`).
 */
interface NativeProgressListener {
    fun onProgress(progress: Int, backend: String, tileSize: Int)
}

/**
 * Constructed directly from C++ (`enhancer_jni.cpp` `build_native_result`).
 * Constructor signature MUST remain `(IIILjava/lang/String;ZILjava/lang/String;)V`.
 */
data class NativeResult(
    val statusCode: Int,
    val outWidth: Int,
    val outHeight: Int,
    val backendUsed: String,
    val usedCpuFallback: Boolean,
    val finalTileSize: Int,
    val errorDetail: String
)

enum class NativeStatusCode(val code: Int) {
    OK(0),
    CANCELLED(1),
    ERR_INVALID_ARGS(-1),
    ERR_BITMAP_LOCK(-2),
    ERR_ASSET_LOAD(-3),
    ERR_OUT_OF_MEMORY(-4),
    ERR_INFERENCE(-5),
    ERR_ENCODE_WRITE(-6),
    ERR_EXCEPTION(-7);

    companion object {
        fun fromCode(code: Int): NativeStatusCode {
            return entries.firstOrNull { it.code == code } ?: ERR_EXCEPTION
        }
    }
}

data class NativeRunOutcome(
    val status: NativeStatusCode,
    val outWidth: Int,
    val outHeight: Int,
    val backendUsed: String,
    val usedCpuFallback: Boolean,
    val finalTileSize: Int,
    val errorDetail: String
)

object NativeEnhancer {
    init {
        System.loadLibrary("enhancer")
    }

    external fun nativeResetCancel()

    external fun nativeCancel()

    external fun nativeQueryGpuInfo(): String

    external fun nativeComputeDimensions(
        srcW: Int,
        srcH: Int,
        modelScale: Int,
        targetLongSide: Int,
        maxQuality: Boolean
    ): IntArray

    external fun nativeResizeBitmapBicubic(
        srcBitmap: Bitmap,
        roiX0: Double,
        roiY0: Double,
        roiX1: Double,
        roiY1: Double,
        dstBitmap: Bitmap
    ): Boolean

    external fun nativeUpscaleImage(
        assetManager: AssetManager,
        srcBitmap: Bitmap,
        paramAssetPath: String,
        binAssetPath: String,
        modelScale: Int,
        targetLongSide: Int,
        tileSize: Int,
        disableFp16: Boolean,
        maxQuality: Boolean,
        ttaX8: Boolean,
        sharpness: Int,
        outputFormat: Int,
        jpegQuality: Int,
        outputFd: Int,
        progressListener: NativeProgressListener?
    ): NativeResult?

    fun getVulkanDeviceName(): String {
        val raw = runCatching { nativeQueryGpuInfo().trim() }.getOrDefault("")
        if (raw.isBlank() || raw.equals("CPU only", ignoreCase = true)) {
            return ""
        }
        return raw.removePrefix("Vulkan:").trim()
    }

    fun resetCancelFlag() {
        runCatching { nativeResetCancel() }
    }

    fun requestCancel() {
        runCatching { nativeCancel() }
    }

    fun resizeBitmapBicubic(
        srcBitmap: Bitmap,
        roiX0: Double,
        roiY0: Double,
        roiX1: Double,
        roiY1: Double,
        dstBitmap: Bitmap
    ): Boolean {
        return runCatching {
            nativeResizeBitmapBicubic(srcBitmap, roiX0, roiY0, roiX1, roiY1, dstBitmap)
        }.getOrDefault(false)
    }

    fun processBitmapToFd(
        assetManager: AssetManager,
        sourceBitmap: Bitmap,
        paramAssetPath: String,
        binAssetPath: String,
        modelScale: Int,
        targetLongSide: Int,
        tileSize: Int,
        disableFp16: Boolean,
        maxQuality: Boolean,
        ttaX8: Boolean,
        sharpness: Int,
        outputFormatCode: Int,
        jpegQuality: Int = 98,
        detachedOutputFd: Int,
        onProgressUpdate: (progress: Int, backend: String, tileSize: Int) -> Unit
    ): NativeRunOutcome {
        var latestBackend = ""
        var latestTile = if (tileSize > 0) tileSize else 200

        val listener = object : NativeProgressListener {
            override fun onProgress(progress: Int, backend: String, tileSize: Int) {
                latestBackend = backend
                latestTile = tileSize
                onProgressUpdate(progress, backend, tileSize)
            }
        }

        val res = nativeUpscaleImage(
            assetManager = assetManager,
            srcBitmap = sourceBitmap,
            paramAssetPath = paramAssetPath,
            binAssetPath = binAssetPath,
            modelScale = modelScale,
            targetLongSide = targetLongSide,
            tileSize = tileSize,
            disableFp16 = disableFp16,
            maxQuality = maxQuality,
            ttaX8 = ttaX8,
            sharpness = sharpness.coerceIn(0, 100),
            outputFormat = outputFormatCode,
            jpegQuality = jpegQuality.coerceIn(90, 100),
            outputFd = detachedOutputFd,
            progressListener = listener
        )

        if (res == null) {
            return NativeRunOutcome(
                status = NativeStatusCode.ERR_EXCEPTION,
                outWidth = 0,
                outHeight = 0,
                backendUsed = latestBackend.ifBlank { "CPU" },
                usedCpuFallback = false,
                finalTileSize = latestTile,
                errorDetail = "JNI returned null NativeResult"
            )
        }

        return NativeRunOutcome(
            status = NativeStatusCode.fromCode(res.statusCode),
            outWidth = res.outWidth,
            outHeight = res.outHeight,
            backendUsed = res.backendUsed.ifBlank { latestBackend.ifBlank { "CPU" } },
            usedCpuFallback = res.usedCpuFallback,
            finalTileSize = if (res.finalTileSize > 0) res.finalTileSize else latestTile,
            errorDetail = res.errorDetail
        )
    }
}
