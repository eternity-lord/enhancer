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
        fileSlug = "animevideov3-x2",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x2,
        descriptionRes = R.string.model_fast_x2_desc
    ),
    FAST_X3(
        paramAssetPath = "models/realesr-animevideov3-x3.param",
        binAssetPath = "models/realesr-animevideov3-x3.bin",
        scale = 3,
        fileSlug = "animevideov3-x3",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x3,
        descriptionRes = R.string.model_fast_x3_desc
    ),
    FAST_X4(
        paramAssetPath = "models/realesr-animevideov3-x4.param",
        binAssetPath = "models/realesr-animevideov3-x4.bin",
        scale = 4,
        fileSlug = "animevideov3-x4",
        approxWeightsMb = 2,
        labelRes = R.string.model_fast_x4,
        descriptionRes = R.string.model_fast_x4_desc
    );
}

enum class TargetPreset(val targetLongSide: Int) {
    NATIVE(0),
    UHD_4K(3840),
    UHD_8K(7680);

    fun fileSlug(modelScale: Int): String = when (this) {
        NATIVE -> "x$modelScale"
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
    val model: UpscaleModel = UpscaleModel.PHOTO_X4,
    val target: TargetPreset = TargetPreset.UHD_4K,
    val format: OutputFormat = OutputFormat.JPEG,
    val tileSize: TileSizeOption = TileSizeOption.AUTO,
    val disableFp16: Boolean = false,
    val languageMode: AppLanguageMode = AppLanguageMode.SYSTEM
)

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
    val postUpscaled: Boolean
)

fun computeResolutionPlan(
    srcWidth: Int,
    srcHeight: Int,
    model: UpscaleModel,
    target: TargetPreset
): ResolutionPlan {
    if (srcWidth <= 0 || srcHeight <= 0) {
        return ResolutionPlan(0, 0, 0, 0, 0, 0, 0, 0, preDownscaled = false, postUpscaled = false)
    }
    val m = model.scale
    val targetLong = target.targetLongSide
    if (targetLong <= 0) {
        val outW = srcWidth * m
        val outH = srcHeight * m
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
            postUpscaled = false
        )
    }

    val srcLong = max(srcWidth, srcHeight)
    val nativeLong = srcLong.toLong() * m.toLong()
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
        val outW = workW * m
        val outH = workH * m
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
            postUpscaled = false
        )
    } else {
        val modelW = srcWidth * m
        val modelH = srcHeight * m
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
            postUpscaled = true
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
    val status: QueueItemStatus = QueueItemStatus.WAITING,
    val progress: Int = 0,
    val activeBackend: String = "",
    val activeTileSize: Int = 200,
    val usedCpuFallback: Boolean = false,
    val outputUri: Uri? = null,
    val outputFileName: String = "",
    val outputWidth: Int = 0,
    val outputHeight: Int = 0,
    val outputMimeType: String = "image/jpeg",
    val modelUsed: UpscaleModel? = null,
    val targetUsed: TargetPreset? = null,
    val elapsedSeconds: Float = 0f,
    val errorKind: QueueErrorKind = QueueErrorKind.NONE,
    val errorDetail: String = ""
)
