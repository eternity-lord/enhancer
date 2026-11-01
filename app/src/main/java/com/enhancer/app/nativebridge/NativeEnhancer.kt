package com.enhancer.app.nativebridge

import android.content.res.AssetManager
import android.graphics.Bitmap

interface NativeProgressCallback {
    fun onProgress(progress: Int, backend: String, tileSize: Int)
    fun onFinishedMetadata(
        outWidth: Int,
        outHeight: Int,
        backend: String,
        usedCpuFallback: Boolean,
        finalTileSize: Int,
        errorDetail: String
    )
}

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

    external fun nativeGetVulkanInfo(): String

    external fun nativeResetCancel()

    external fun nativeCancel()

    external fun nativeProcessBitmap(
        assetManager: AssetManager,
        sourceBitmap: Bitmap,
        paramAssetPath: String,
        binAssetPath: String,
        modelScale: Int,
        targetLongSide: Int,
        tileSize: Int,
        disableFp16: Boolean,
        outputFormat: Int,
        jpegQuality: Int,
        outputFd: Int,
        callback: NativeProgressCallback?
    ): Int

    fun getVulkanDeviceName(): String {
        return runCatching { nativeGetVulkanInfo().trim() }.getOrDefault("")
    }

    fun resetCancelFlag() {
        runCatching { nativeResetCancel() }
    }

    fun requestCancel() {
        runCatching { nativeCancel() }
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
        outputFormatCode: Int,
        jpegQuality: Int = 95,
        detachedOutputFd: Int,
        onProgressUpdate: (progress: Int, backend: String, tileSize: Int) -> Unit
    ): NativeRunOutcome {
        var metaWidth = 0
        var metaHeight = 0
        var metaBackend = ""
        var metaFallback = false
        var metaTile = if (tileSize > 0) tileSize else 200
        var metaError = ""

        val callback = object : NativeProgressCallback {
            override fun onProgress(progress: Int, backend: String, tileSize: Int) {
                metaBackend = backend
                metaTile = tileSize
                onProgressUpdate(progress, backend, tileSize)
            }

            override fun onFinishedMetadata(
                outWidth: Int,
                outHeight: Int,
                backend: String,
                usedCpuFallback: Boolean,
                finalTileSize: Int,
                errorDetail: String
            ) {
                metaWidth = outWidth
                metaHeight = outHeight
                if (backend.isNotBlank()) {
                    metaBackend = backend
                }
                metaFallback = usedCpuFallback
                metaTile = finalTileSize
                metaError = errorDetail
            }
        }

        val rawStatus = nativeProcessBitmap(
            assetManager = assetManager,
            sourceBitmap = sourceBitmap,
            paramAssetPath = paramAssetPath,
            binAssetPath = binAssetPath,
            modelScale = modelScale,
            targetLongSide = targetLongSide,
            tileSize = tileSize,
            disableFp16 = disableFp16,
            outputFormat = outputFormatCode,
            jpegQuality = jpegQuality,
            outputFd = detachedOutputFd,
            callback = callback
        )

        return NativeRunOutcome(
            status = NativeStatusCode.fromCode(rawStatus),
            outWidth = metaWidth,
            outHeight = metaHeight,
            backendUsed = metaBackend.ifBlank { "CPU" },
            usedCpuFallback = metaFallback,
            finalTileSize = metaTile,
            errorDetail = metaError
        )
    }
}
