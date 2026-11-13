package com.enhancer.app.model

import android.graphics.Bitmap
import android.net.Uri
import androidx.annotation.StringRes
import com.enhancer.app.R
import kotlin.math.max
import kotlin.math.roundToInt

enum class UpscaleModel(
    val paramAssetPath: String,
    val binAssetPath: String,
    val scale: Int,
    val fileSlug: String,
    val approxWeightsMb: Int,
    @StringRes val labelRes: Int,
    @StringRes val descriptionRes: Int
) {
    PHOTO_X4(
        paramAssetPath = "models/realesrgan-x4plus.param",
        binAssetPath = "models/realesrgan-x4plus.bin",
        scale = 4,
        fileSlug = "x4plus",
        approxWeightsMb = 34,
        labelRes = R.string.model_photo,
        descriptionRes = R.string.model_photo_desc
    ),
    GENERAL_X4_V3(
        paramAssetPath = "models/realesr-general-x4v3.param",
        binAssetPath = "models/realesr-general-x4v3.bin",
        scale = 4,
        fileSlug = "general-v3",
        approxWeightsMb = 3,
        labelRes = R.string.model_general_v3,
        descriptionRes = R.string.model_general_v3_desc
    ),
    GENERAL_WDN_X4_V3(
        paramAssetPath = "models/realesr-general-wdn-x4v3.param",
        binAssetPath = "models/realesr-general-wdn-x4v3.bin",
        scale = 4,
        fileSlug = "general-wdn",
        approxWeightsMb = 3,
        labelRes = R.string.model_general_wdn_v3,
        descriptionRes = R.string.model_general_wdn_v3_desc
    ),
    LSDIR_COMPACT_X4(
        paramAssetPath = "models/lsdir-compact-x4.param",
        binAssetPath = "models/lsdir-compact-x4.bin",
        scale = 4,
        fileSlug = "lsdir-x4",
        approxWeightsMb = 2,
        labelRes = R.string.model_lsdir_x4,
        descriptionRes = R.string.model_lsdir_x4_desc
    ),
    ANIME_X4(
        paramAssetPath = "models/realesrgan-x4plus-anime.param",
        binAssetPath = "models/realesrgan-x4plus-anime.bin",
        scale = 4,
        fileSlug = "x4plus-anime",
        approxWeightsMb = 10,
        labelRes = R.string.model_anime,
        descriptionRes = R.string.model_anime_desc
    ),
    FAST_X2(
        paramAssetPath = "models/realesr-animevideov3-x2.param",
        binAssetPath = "models/realesr-animevideov3-x2.bin",
        scale = 2,
        fileSlug = "compact-x2",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x2,
        descriptionRes = R.string.model_fast_x2_desc
    ),
    FAST_X3(
        paramAssetPath = "models/realesr-animevideov3-x3.param",
        binAssetPath = "models/realesr-animevideov3-x3.bin",
        scale = 3,
        fileSlug = "compact-x3",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x3,
        descriptionRes = R.string.model_fast_x3_desc
    ),
    FAST_X4(
        paramAssetPath = "models/realesr-animevideov3-x4.param",
        binAssetPath = "models/realesr-animevideov3-x4.bin",
        scale = 4,
        fileSlug = "compact-x4",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x4,
        descriptionRes = R.string.model_fast_x4_desc
    );
}

/**
 * Let's Enhance (letsenhance.io) 7 Studio AI Enhancement Modes.
 * Selecting a mode applies optimal defaults while leaving every sub-control 100% unlocked.
 */
enum class EnhanceModePreset(
    val nativeCode: Int,
    val fileSlug: String,
    @StringRes val labelRes: Int,
    @StringRes val descriptionRes: Int,
    val defaultModel: UpscaleModel,
    val defaultFaces: Boolean,
    val defaultFaceStrength: Int,
    val defaultLightAi: Boolean,
    val defaultLightAiStrength: Int,
    val defaultDenoise: Int,
    val defaultSharpness: Int,
    val defaultUltraIntensity: Int = 65,
    val defaultUltraSize: Int = 45
) {
    PRIME(
        nativeCode = 0,
        fileSlug = "prime",
        labelRes = R.string.mode_prime,
        descriptionRes = R.string.mode_prime_desc,
        defaultModel = UpscaleModel.PHOTO_X4,
        defaultFaces = true,
        defaultFaceStrength = 68,
        defaultLightAi = false,
        defaultLightAiStrength = 40,
        defaultDenoise = 10,
        defaultSharpness = 8
    ),
    GENTLE(
        nativeCode = 1,
        fileSlug = "gentle",
        labelRes = R.string.mode_gentle,
        descriptionRes = R.string.mode_gentle_desc,
        defaultModel = UpscaleModel.LSDIR_COMPACT_X4,
        defaultFaces = false,
        defaultFaceStrength = 60,
        defaultLightAi = false,
        defaultLightAiStrength = 35,
        defaultDenoise = 0,
        defaultSharpness = 5
    ),
    BALANCED(
        nativeCode = 2,
        fileSlug = "balanced",
        labelRes = R.string.mode_balanced,
        descriptionRes = R.string.mode_balanced_desc,
        defaultModel = UpscaleModel.GENERAL_X4_V3,
        defaultFaces = true,
        defaultFaceStrength = 70,
        defaultLightAi = true,
        defaultLightAiStrength = 45,
        defaultDenoise = 18,
        defaultSharpness = 12
    ),
    STRONG(
        nativeCode = 3,
        fileSlug = "strong",
        labelRes = R.string.mode_strong,
        descriptionRes = R.string.mode_strong_desc,
        defaultModel = UpscaleModel.GENERAL_WDN_X4_V3,
        defaultFaces = true,
        defaultFaceStrength = 80,
        defaultLightAi = true,
        defaultLightAiStrength = 60,
        defaultDenoise = 60,
        defaultSharpness = 18
    ),
    ULTRA(
        nativeCode = 4,
        fileSlug = "ultra",
        labelRes = R.string.mode_ultra,
        descriptionRes = R.string.mode_ultra_desc,
        defaultModel = UpscaleModel.PHOTO_X4,
        defaultFaces = true,
        defaultFaceStrength = 75,
        defaultLightAi = true,
        defaultLightAiStrength = 50,
        defaultDenoise = 15,
        defaultSharpness = 20,
        defaultUltraIntensity = 65,
        defaultUltraSize = 45
    ),
    DIGITAL_ART(
        nativeCode = 5,
        fileSlug = "digital-art",
        labelRes = R.string.mode_digital_art,
        descriptionRes = R.string.mode_digital_art_desc,
        defaultModel = UpscaleModel.ANIME_X4,
        defaultFaces = false,
        defaultFaceStrength = 60,
        defaultLightAi = false,
        defaultLightAiStrength = 35,
        defaultDenoise = 12,
        defaultSharpness = 14
    ),
    OLD_PHOTO(
        nativeCode = 6,
        fileSlug = "old-photo",
        labelRes = R.string.mode_old_photo,
        descriptionRes = R.string.mode_old_photo_desc,
        defaultModel = UpscaleModel.PHOTO_X4,
        defaultFaces = true,
        defaultFaceStrength = 82,
        defaultLightAi = true,
        defaultLightAiStrength = 40,
        defaultDenoise = 35,
        defaultSharpness = 10
    );
}

enum class TargetPreset(val targetLongSide: Int) {
    NATIVE(0),
    QHD_2K(2560),
    UHD_4K(3840),
    UHD_8K(7680);

    fun fileSlug(modelScale: Int): String = when (this) {
        NATIVE -> "x$modelScale"
        QHD_2K -> "2K"
        UHD_4K -> "4K"
        UHD_8K -> "8K"
    }
}

enum class OutputFormat(
    val nativeCode: Int,
    val extension: String,
    val mimeType: String
) {
    JPEG(nativeCode = 0, extension = "jpg", mimeType = "image/jpeg"),
    PNG(nativeCode = 1, extension = "png", mimeType = "image/png");
}

enum class TileSizeOption(
    val nativeArg: Int,
    val effectivePixels: Int
) {
    AUTO(nativeArg = 0, effectivePixels = 200),
    TILE_256(nativeArg = 256, effectivePixels = 256),
    TILE_192(nativeArg = 192, effectivePixels = 192),
    TILE_128(nativeArg = 128, effectivePixels = 128),
    TILE_64(nativeArg = 64, effectivePixels = 64);
}

enum class AppLanguageMode(val tag: String) {
    SYSTEM(""),
    EN("en"),
    FA("fa");
}

data class EnhancerSettings(
    val enhanceMode: EnhanceModePreset = EnhanceModePreset.BALANCED,
    val model: UpscaleModel = EnhanceModePreset.BALANCED.defaultModel,
    val target: TargetPreset = TargetPreset.UHD_4K,
    val enhanceFaces: Boolean = EnhanceModePreset.BALANCED.defaultFaces,
    val faceStrength: Int = EnhanceModePreset.BALANCED.defaultFaceStrength, // 0..100
    val lightAi: Boolean = EnhanceModePreset.BALANCED.defaultLightAi,
    val lightAiStrength: Int = EnhanceModePreset.BALANCED.defaultLightAiStrength, // 0..100
    val denoiseStrength: Int = EnhanceModePreset.BALANCED.defaultDenoise, // 0..100
    val ultraIntensity: Int = 65, // 0..100 (Ultra mode)
    val ultraSize: Int = 45, // 0..100 (Ultra mode "Size of changes")
    val maxQuality: Boolean = false,
    val ttaX8: Boolean = false,
    val safeMode: Boolean = false, // Legacy flag; UI uses unlocked forceCpu + applyLowRamPreset()
    val forceCpu: Boolean = false, // True = CPU Precision backend, False = Auto GPU (Vulkan + fallback)
    val sharpness: Int = EnhanceModePreset.BALANCED.defaultSharpness, // 0..100 (0 = off)
    val format: OutputFormat = OutputFormat.JPEG,
    val jpegQuality: Int = 98, // 90..100, always 4:4:4 full chroma
    val dpi: Int = 300, // Print-ready 300 DPI metadata
    val tileSize: TileSizeOption = TileSizeOption.AUTO,
    val disableFp16: Boolean = false,
    val languageMode: AppLanguageMode = AppLanguageMode.SYSTEM
) {
    val effectiveMaxQuality: Boolean
        get() = if (safeMode) false else maxQuality

    val effectiveTtaX8: Boolean
        get() = if (safeMode) false else ttaX8

    val effectiveTileSize: TileSizeOption
        get() = if (safeMode) TileSizeOption.TILE_64 else tileSize

    val usesCpuOnly: Boolean
        get() = safeMode || forceCpu

    fun withModePreset(preset: EnhanceModePreset): EnhancerSettings = copy(
        enhanceMode = preset,
        model = preset.defaultModel,
        enhanceFaces = preset.defaultFaces,
        faceStrength = preset.defaultFaceStrength,
        lightAi = preset.defaultLightAi,
        lightAiStrength = preset.defaultLightAiStrength,
        denoiseStrength = preset.defaultDenoise,
        sharpness = preset.defaultSharpness,
        ultraIntensity = preset.defaultUltraIntensity,
        ultraSize = preset.defaultUltraSize
    )

    fun withLowRamPreset(): EnhancerSettings = copy(
        forceCpu = true,
        safeMode = false,
        tileSize = TileSizeOption.TILE_64,
        maxQuality = false,
        ttaX8 = false
    )
}

data class ResolutionPlan(
    val srcWidth: Int,
    val srcHeight: Int,
    val workWidth: Int,
    val workHeight: Int,
    val modelOutWidth: Int,
    val modelOutHeight: Int,
    val finalWidth: Int,
    val finalHeight: Int,
    val preDownscaled: Boolean,
    val postUpscaled: Boolean,
    val postDownscaled: Boolean = false
)

fun computeResolutionPlan(
    srcWidth: Int,
    srcHeight: Int,
    model: UpscaleModel,
    target: TargetPreset,
    maxQuality: Boolean = false
): ResolutionPlan {
    if (srcWidth <= 0 || srcHeight <= 0) {
        return ResolutionPlan(
            0, 0, 0, 0, 0, 0, 0, 0,
            preDownscaled = false,
            postUpscaled = false,
            postDownscaled = false
        )
    }
    val m = model.scale
    val mLong = m.toLong()
    val scaledWLong = srcWidth.toLong() * mLong
    val scaledHLong = srcHeight.toLong() * mLong
    if (scaledWLong > (Int.MAX_VALUE / 4).toLong() || scaledHLong > (Int.MAX_VALUE / 4).toLong()) {
        return ResolutionPlan(
            0, 0, 0, 0, 0, 0, 0, 0,
            preDownscaled = false,
            postUpscaled = false,
            postDownscaled = false
        )
    }

    val targetLong = target.targetLongSide
    if (targetLong <= 0) {
        val outW = scaledWLong.toInt()
        val outH = scaledHLong.toInt()
        return ResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            workWidth = srcWidth,
            workHeight = srcHeight,
            modelOutWidth = outW,
            modelOutHeight = outH,
            finalWidth = outW,
            finalHeight = outH,
            preDownscaled = false,
            postUpscaled = false,
            postDownscaled = false
        )
    }

    if (maxQuality) {
        val modelW = scaledWLong.toInt()
        val modelH = scaledHLong.toInt()
        val finalW: Int
        val finalH: Int
        if (srcWidth >= srcHeight) {
            finalW = targetLong
            finalH = max(1, (srcHeight.toDouble() * targetLong / srcWidth.toDouble()).roundToInt())
        } else {
            finalH = targetLong
            finalW = max(1, (srcWidth.toDouble() * targetLong / srcHeight.toDouble()).roundToInt())
        }
        val modelLong = max(modelW, modelH)
        return ResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            workWidth = srcWidth,
            workHeight = srcHeight,
            modelOutWidth = modelW,
            modelOutHeight = modelH,
            finalWidth = finalW,
            finalHeight = finalH,
            preDownscaled = false,
            postUpscaled = modelLong < targetLong,
            postDownscaled = modelLong > targetLong
        )
    }

    val srcLong = max(srcWidth, srcHeight)
    val nativeLong = srcLong.toLong() * mLong
    return if (nativeLong >= targetLong.toLong()) {
        val workLong = max(1, targetLong / m)
        val workW: Int
        val workH: Int
        if (srcWidth >= srcHeight) {
            workW = workLong
            workH = max(1, (srcHeight.toDouble() * workLong / srcWidth.toDouble()).roundToInt())
        } else {
            workH = workLong
            workW = max(1, (srcWidth.toDouble() * workLong / srcHeight.toDouble()).roundToInt())
        }
        val outW = (workW.toLong() * mLong).toInt()
        val outH = (workH.toLong() * mLong).toInt()
        ResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            workWidth = workW,
            workHeight = workH,
            modelOutWidth = outW,
            modelOutHeight = outH,
            finalWidth = outW,
            finalHeight = outH,
            preDownscaled = (workW < srcWidth || workH < srcHeight),
            postUpscaled = false,
            postDownscaled = false
        )
    } else {
        val modelW = scaledWLong.toInt()
        val modelH = scaledHLong.toInt()
        val finalW: Int
        val finalH: Int
        if (srcWidth >= srcHeight) {
            finalW = targetLong
            finalH = max(1, (srcHeight.toDouble() * targetLong / srcWidth.toDouble()).roundToInt())
        } else {
            finalH = targetLong
            finalW = max(1, (srcWidth.toDouble() * targetLong / srcHeight.toDouble()).roundToInt())
        }
        ResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            workWidth = srcWidth,
            workHeight = srcHeight,
            modelOutWidth = modelW,
            modelOutHeight = modelH,
            finalWidth = finalW,
            finalHeight = finalH,
            preDownscaled = false,
            postUpscaled = true,
            postDownscaled = false
        )
    }
}

enum class QueueItemStatus {
    WAITING,
    RUNNING,
    DONE,
    FAILED,
    CANCELLED
}

enum class QueueErrorKind {
    NONE,
    DECODE_SOURCE,
    MEDIASTORE_CREATE,
    OUT_OF_MEMORY,
    ASSET_LOAD,
    BITMAP_LOCK,
    VULKAN_INIT,
    VK_ALLOCATOR,
    NCNN_LOAD_PARAM,
    NCNN_LOAD_MODEL,
    NCNN_INPUT,
    NCNN_EXTRACT,
    INFERENCE,
    ENCODE_WRITE,
    UNKNOWN
}

data class QueueItem(
    val id: String,
    val sourceUri: Uri,
    val displayName: String,
    val baseName: String,
    val sourceWidth: Int,
    val sourceHeight: Int,
    val exifOrientation: Int,
    val thumbnail: Bitmap?,
    val settingsOverride: EnhancerSettings? = null,
    val appliedSettings: EnhancerSettings? = null,
    val status: QueueItemStatus = QueueItemStatus.WAITING,
    val progress: Int = 0,
    val activeBackend: String = "",
    val activeTileSize: Int = 200,
    val usedCpuFallback: Boolean = false,
    val fallbackSummary: String = "",
    val facesDetected: Int = 0,
    val facesRestored: Int = 0,
    val outputUri: Uri? = null,
    val outputFileName: String = "",
    val outputWidth: Int = 0,
    val outputHeight: Int = 0,
    val outputMimeType: String = "image/jpeg",
    val modelUsed: UpscaleModel? = null,
    val targetUsed: TargetPreset? = null,
    val elapsedSeconds: Float = 0f,
    val errorKind: QueueErrorKind = QueueErrorKind.NONE,
    val nativeStatusCode: Int = 0,
    val errorDetail: String = ""
)
