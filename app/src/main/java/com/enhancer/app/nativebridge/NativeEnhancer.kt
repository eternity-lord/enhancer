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
 * Constructor signature MUST remain `(IIILjava/lang/String;ZILjava/lang/String;Ljava/lang/String;)V`.
 */
data class NativeResult(
    val statusCode: Int,
    val outWidth: Int,
    val outHeight: Int,
    val backendUsed: String,
    val usedCpuFallback: Boolean,
    val finalTileSize: Int,
    val fallbackSummary: String,
    val errorDetail: String
)

/**
 * Distinct native status codes mirroring `enhancer::EnhancerStatus` in `enhancer_types.h`.
 * `ERR_OUT_OF_MEMORY` (-4) is strictly reserved ONLY for `std::bad_alloc` or `malloc` failure.
 */
enum class NativeStatusCode(val code: Int) {
    OK(0),
    CANCELLED(1),
    ERR_INVALID_ARGS(-1),
    ERR_BITMAP_LOCK(-2),
    ERR_ASSET_LOAD(-3),
    ERR_OUT_OF_MEMORY(-4),
    ERR_INFERENCE(-5),
    ERR_ENCODE_WRITE(-6),
    ERR_EXCEPTION(-7),
    ERR_VULKAN_INIT(-8),
    ERR_VK_ALLOCATOR(-9),
    ERR_NCNN_LOAD_PARAM(-10),
    ERR_NCNN_LOAD_MODEL(-11),
    ERR_NCNN_INPUT(-12),
    ERR_NCNN_EXTRACT(-13);

    companion object {
        fun fromCode(code: Int): NativeStatusCode {
            return entries.firstOrNull { it.code == code } ?: ERR_EXCEPTION
        }
    }
}

data class NativeRunOutcome(
    val status: NativeStatusCode,
    val rawStatusCode: Int,
    val outWidth: Int,
    val outHeight: Int,
    val backendUsed: String,
    val usedCpuFallback: Boolean,
    val finalTileSize: Int,
    val fallbackSummary: String,
    val errorDetail: String
)

data class NativeSelfTestResult(
    val vulkanPassed: Boolean,
    val vulkanElapsedMs: Long,
    val vulkanDetail: String,
    val cpuPassed: Boolean,
    val cpuElapsedMs: Long,
    val cpuDetail: String,
    val fullLog: String
)

data class Fp16PolicyCalibration(
    val recommendDisableFp16: Boolean,
    val vulkanAvailable: Boolean,
    val isAdreno: Boolean,
    val reason: String
)

data class SeamTestRowItem(
    val backend: String,
    val fp16Label: String,
    val maxQualityLabel: String,
    val tileSize: Int,
    val statusLabel: String,
    val meanBndVsInt: String,
    val maxBndVsInt: String,
    val neighborMeanDiff: String,
    val elapsedMs: Long,
    val gridNote: String
)

data class NativeSeamTestResult(
    val statusCode: Int,
    val overallPass: Boolean,
    val summary: String,
    val fp16PolicyReason: String,
    val formattedTable: String,
    val rows: List<SeamTestRowItem>
)

object NativeEnhancer {
    init {
        System.loadLibrary("enhancer")
    }

    external fun nativeResetCancel()

    external fun nativeCancel()

    external fun nativeCancelSeamTest()

    external fun nativeShouldDefaultDisableFp16(): Boolean

    external fun nativeCalibrateFp16Policy(assetManager: AssetManager): Array<String>?

    external fun nativeQueryGpuInfo(): String

    external fun nativeQueryGpuDiagnostics(): String

    external fun nativeRunSelfTest(assetManager: AssetManager): Array<String>?

    external fun nativeRunSeamTest(
        assetManager: AssetManager,
        progressListener: NativeProgressListener?
    ): Array<String>?

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
        safeMode: Boolean,
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

    fun getGpuDiagnosticsSummary(): String {
        return runCatching { nativeQueryGpuDiagnostics().trim() }
            .getOrDefault("Vulkan diagnostics unavailable")
    }

    fun runSelfTest(assetManager: AssetManager): NativeSelfTestResult {
        val arr = runCatching { nativeRunSelfTest(assetManager) }.getOrNull()
        if (arr == null || arr.size < 7) {
            val msg = "JNI nativeRunSelfTest returned null or incomplete result"
            return NativeSelfTestResult(
                vulkanPassed = false,
                vulkanElapsedMs = 0L,
                vulkanDetail = msg,
                cpuPassed = false,
                cpuElapsedMs = 0L,
                cpuDetail = msg,
                fullLog = msg
            )
        }
        return NativeSelfTestResult(
            vulkanPassed = arr[0] == "PASS",
            vulkanElapsedMs = arr[1].toLongOrNull() ?: 0L,
            vulkanDetail = arr[2],
            cpuPassed = arr[3] == "PASS",
            cpuElapsedMs = arr[4].toLongOrNull() ?: 0L,
            cpuDetail = arr[5],
            fullLog = arr[6]
        )
    }

    fun resetCancelFlag() {
        runCatching { nativeResetCancel() }
    }

    fun requestCancel() {
        runCatching { nativeCancel() }
    }

    fun cancelSeamTest() {
        runCatching { nativeCancelSeamTest() }
    }

    fun shouldDefaultDisableFp16(): Boolean {
        return runCatching { nativeShouldDefaultDisableFp16() }.getOrDefault(false)
    }

    fun calibrateFp16Policy(assetManager: AssetManager): Fp16PolicyCalibration {
        val arr = runCatching { nativeCalibrateFp16Policy(assetManager) }.getOrNull()
        if (arr == null || arr.size < 4) {
            val defOff = shouldDefaultDisableFp16()
            return Fp16PolicyCalibration(
                recommendDisableFp16 = defOff,
                vulkanAvailable = false,
                isAdreno = defOff,
                reason = if (defOff) "FP16 default: OFF (Qualcomm Adreno hardware policy)" else "FP16 default: ON"
            )
        }
        return Fp16PolicyCalibration(
            recommendDisableFp16 = arr[0] == "1",
            vulkanAvailable = arr[1] == "1",
            isAdreno = arr[2] == "1",
            reason = arr[3]
        )
    }

    fun runSeamTest(
        assetManager: AssetManager,
        onProgressUpdate: (doneCases: Int, totalCases: Int, label: String) -> Unit
    ): NativeSeamTestResult {
        val listener = object : NativeProgressListener {
            override fun onProgress(progress: Int, backend: String, tileSize: Int) {
                onProgressUpdate(progress, tileSize, backend)
            }
        }
        val arr = runCatching { nativeRunSeamTest(assetManager, listener) }.getOrNull()
        if (arr == null || arr.size < 5) {
            val msg = "JNI nativeRunSeamTest returned null or incomplete result"
            return NativeSeamTestResult(
                statusCode = NativeStatusCode.ERR_EXCEPTION.code,
                overallPass = false,
                summary = msg,
                fp16PolicyReason = "",
                formattedTable = msg,
                rows = emptyList()
            )
        }

        val parsedRows = buildList {
            for (i in 5 until arr.size) {
                val parts = arr[i].split('\t')
                if (parts.size >= 10) {
                    add(
                        SeamTestRowItem(
                            backend = parts[0],
                            fp16Label = parts[1],
                            maxQualityLabel = parts[2],
                            tileSize = parts[3].toIntOrNull() ?: 200,
                            statusLabel = parts[4],
                            meanBndVsInt = parts[5],
                            maxBndVsInt = parts[6],
                            neighborMeanDiff = parts[7],
                            elapsedMs = parts[8].toLongOrNull() ?: 0L,
                            gridNote = parts[9]
                        )
                    )
                }
            }
        }

        return NativeSeamTestResult(
            statusCode = arr[0].toIntOrNull() ?: 0,
            overallPass = arr[1] == "PASS",
            summary = arr[2],
            fp16PolicyReason = arr[3],
            formattedTable = arr[4],
            rows = parsedRows
        )
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
        safeMode: Boolean = false,
        sharpness: Int,
        outputFormatCode: Int,
        jpegQuality: Int = 98,
        detachedOutputFd: Int,
        onProgressUpdate: (progress: Int, backend: String, tileSize: Int) -> Unit
    ): NativeRunOutcome {
        var latestBackend = if (safeMode) "CPU" else ""
        var latestTile = if (safeMode) 64 else if (tileSize > 0) tileSize else 200

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
            tileSize = if (safeMode) 64 else tileSize,
            disableFp16 = disableFp16,
            maxQuality = if (safeMode) false else maxQuality,
            ttaX8 = if (safeMode) false else ttaX8,
            safeMode = safeMode,
            sharpness = sharpness.coerceIn(0, 100),
            outputFormat = outputFormatCode,
            jpegQuality = jpegQuality.coerceIn(90, 100),
            outputFd = detachedOutputFd,
            progressListener = listener
        )

        if (res == null) {
            return NativeRunOutcome(
                status = NativeStatusCode.ERR_EXCEPTION,
                rawStatusCode = NativeStatusCode.ERR_EXCEPTION.code,
                outWidth = 0,
                outHeight = 0,
                backendUsed = latestBackend.ifBlank { "CPU" },
                usedCpuFallback = false,
                finalTileSize = latestTile,
                fallbackSummary = "",
                errorDetail = "[code=-7 ERR_EXCEPTION] step=JNI_return | requested_bytes=0 | tile=$latestTile | backend=${latestBackend.ifBlank { "CPU" }} | ncnn_rc=0 | detail=JNI returned null NativeResult"
            )
        }

        return NativeRunOutcome(
            status = NativeStatusCode.fromCode(res.statusCode),
            rawStatusCode = res.statusCode,
            outWidth = res.outWidth,
            outHeight = res.outHeight,
            backendUsed = res.backendUsed.ifBlank { latestBackend.ifBlank { "CPU" } },
            usedCpuFallback = res.usedCpuFallback,
            finalTileSize = if (res.finalTileSize > 0) res.finalTileSize else latestTile,
            fallbackSummary = res.fallbackSummary,
            errorDetail = res.errorDetail
        )
    }
}
