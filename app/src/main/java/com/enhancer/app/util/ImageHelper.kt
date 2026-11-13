package com.enhancer.app.util

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.graphics.Rect
import android.net.Uri
import android.provider.OpenableColumns
import androidx.exifinterface.media.ExifInterface
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
import com.enhancer.app.model.TargetPreset
import com.enhancer.app.model.UpscaleModel
import kotlin.math.max
import kotlin.math.roundToInt

data class InspectedImageInfo(
    val displayName: String,
    val sanitizedBaseName: String,
    val orientedWidth: Int,
    val orientedHeight: Int,
    val exifOrientation: Int,
    val thumbnail: Bitmap?
)

object ImageHelper {

    fun inspectImage(context: Context, uri: Uri): InspectedImageInfo? {
        val resolver = context.contentResolver
        val displayName = queryDisplayName(context, uri)
        val baseName = sanitizeBaseName(displayName)

        val exifOrientation = runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                ExifInterface(stream).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL
                )
            } ?: ExifInterface.ORIENTATION_NORMAL
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)

        val boundsOpts = BitmapFactory.Options().apply {
            inJustDecodeBounds = true
        }
        runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, boundsOpts)
            }
        }

        val rawW = boundsOpts.outWidth
        val rawH = boundsOpts.outHeight
        if (rawW <= 0 || rawH <= 0) {
            return null
        }

        val swapsAxes = isQuarterTurnOrientation(exifOrientation)
        val orientedW = if (swapsAxes) rawH else rawW
        val orientedH = if (swapsAxes) rawW else rawH

        val thumb = decodeDownscaledBitmap(
            context = context,
            uri = uri,
            maxLongSide = 160,
            exifOrientation = exifOrientation
        )

        return InspectedImageInfo(
            displayName = displayName,
            sanitizedBaseName = baseName,
            orientedWidth = orientedW,
            orientedHeight = orientedH,
            exifOrientation = exifOrientation,
            thumbnail = thumb
        )
    }

    /**
     * Decodes full source image into ARGB_8888 (NDK RGBA_8888) and applies EXIF orientation
     * before passing pixels to JNI via AndroidBitmap_lockPixels.
     */
    fun decodeOrientedSourceBitmap(
        context: Context,
        uri: Uri,
        exifOrientation: Int
    ): Bitmap? {
        val opts = BitmapFactory.Options().apply {
            inPreferredConfig = Bitmap.Config.ARGB_8888
            inMutable = false
        }
        val rawBitmap = runCatching {
            context.contentResolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, opts)
            }
        }.getOrNull() ?: return null

        val transformed = applyExifMatrix(rawBitmap, exifOrientation)
        if (transformed.config == Bitmap.Config.ARGB_8888) {
            return transformed
        }

        val argbCopy = transformed.copy(Bitmap.Config.ARGB_8888, false)
        if (argbCopy != transformed) {
            transformed.recycle()
        }
        return argbCopy
    }

    /**
     * Memory-safe downscaled decode for thumbnails and low-res base layers.
     */
    fun decodeDownscaledBitmap(
        context: Context,
        uri: Uri,
        maxLongSide: Int = 1600,
        exifOrientation: Int? = null
    ): Bitmap? {
        val resolver = context.contentResolver
        val actualOrientation = exifOrientation ?: runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                ExifInterface(stream).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL
                )
            } ?: ExifInterface.ORIENTATION_NORMAL
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)

        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, bounds)
            }
        }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            return null
        }

        val rawLong = max(bounds.outWidth, bounds.outHeight)
        var sampleSize = 1
        while (rawLong / (sampleSize * 2) >= maxLongSide) {
            sampleSize *= 2
        }

        val decodeOpts = BitmapFactory.Options().apply {
            inSampleSize = sampleSize
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }

        val sampled = runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                BitmapFactory.decodeStream(stream, null, decodeOpts)
            }
        }.getOrNull() ?: return null

        val oriented = applyExifMatrix(sampled, actualOrientation)
        val curLong = max(oriented.width, oriented.height)
        if (curLong <= maxLongSide) {
            return oriented
        }

        val ratio = maxLongSide.toFloat() / curLong.toFloat()
        val dstW = max(1, (oriented.width * ratio).roundToInt())
        val dstH = max(1, (oriented.height * ratio).roundToInt())
        val scaled = Bitmap.createScaledBitmap(oriented, dstW, dstH, true)
        if (scaled != oriented) {
            oriented.recycle()
        }
        return scaled
    }

    fun buildOutputFileName(
        baseName: String,
        target: TargetPreset,
        model: UpscaleModel,
        format: OutputFormat,
        settings: EnhancerSettings? = null
    ): String {
        val cleanBase = baseName.ifBlank { "image" }
        val targetSlug = target.fileSlug(model.scale)
        val modeSlug = settings?.enhanceMode?.fileSlug?.let { "_$it" } ?: ""
        val extras = buildString {
            if (settings?.enhanceFaces == true) append("_faces")
            if (settings?.lightAi == true) append("_lightAI")
            if (settings?.usesCpuOnly == true) append("_cpu")
            if (settings?.effectiveMaxQuality == true) append("_maxQ")
            if (settings?.effectiveTtaX8 == true) append("_tta8")
            append("_300dpi")
        }
        return "${cleanBase}_${targetSlug}${modeSlug}_${model.fileSlug}${extras}.${format.extension}"
    }

    fun isQuarterTurnOrientation(orientation: Int): Boolean {
        return orientation == ExifInterface.ORIENTATION_ROTATE_90 ||
            orientation == ExifInterface.ORIENTATION_ROTATE_270 ||
            orientation == ExifInterface.ORIENTATION_TRANSPOSE ||
            orientation == ExifInterface.ORIENTATION_TRANSVERSE
    }

    /**
     * Maps a rectangle in EXIF-oriented coordinates `(orientedW x orientedH)` back into
     * unrotated sensor coordinates `(rawW x rawH)` for `BitmapRegionDecoder.decodeRegion`.
     */
    fun mapOrientedRectToRaw(
        orientedRect: Rect,
        orientedW: Int,
        orientedH: Int,
        rawW: Int,
        rawH: Int,
        orientation: Int
    ): Rect {
        val l = orientedRect.left.coerceIn(0, orientedW)
        val t = orientedRect.top.coerceIn(0, orientedH)
        val r = orientedRect.right.coerceIn(l, orientedW)
        val b = orientedRect.bottom.coerceIn(t, orientedH)

        val raw = when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 ->
                Rect(t, rawH - r, b, rawH - l)
            ExifInterface.ORIENTATION_ROTATE_180 ->
                Rect(rawW - r, rawH - b, rawW - l, rawH - t)
            ExifInterface.ORIENTATION_ROTATE_270 ->
                Rect(rawW - b, l, rawW - t, r)
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL ->
                Rect(rawW - r, t, rawW - l, b)
            ExifInterface.ORIENTATION_FLIP_VERTICAL ->
                Rect(l, rawH - b, r, rawH - t)
            ExifInterface.ORIENTATION_TRANSPOSE ->
                Rect(t, l, b, r)
            ExifInterface.ORIENTATION_TRANSVERSE ->
                Rect(rawW - b, rawH - r, rawW - t, rawH - l)
            else ->
                Rect(l, t, r, b)
        }
        return Rect(
            raw.left.coerceIn(0, rawW),
            raw.top.coerceIn(0, rawH),
            raw.right.coerceIn(0, rawW),
            raw.bottom.coerceIn(0, rawH)
        )
    }

    fun applyExifToRegionBitmap(bitmap: Bitmap, orientation: Int): Bitmap {
        return applyExifMatrix(bitmap, orientation)
    }

    private fun queryDisplayName(context: Context, uri: Uri): String {
        runCatching {
            context.contentResolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME),
                null,
                null,
                null
            )?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) {
                    val name = cursor.getString(idx)
                    if (!name.isNullOrBlank()) {
                        return name
                    }
                }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/')?.ifBlank { "image" } ?: "image"
    }

    private fun sanitizeBaseName(displayName: String): String {
        val withoutExt = displayName.substringBeforeLast('.', displayName).trim()
        val cleaned = withoutExt
            .replace(Regex("""[^\p{L}\p{N}._\- ]"""), "_")
            .replace(Regex("""\s+"""), "_")
            .trim('_')
        return cleaned.take(64).ifBlank { "image" }
    }

    private fun applyExifMatrix(bitmap: Bitmap, orientation: Int): Bitmap {
        val matrix = Matrix()
        when (orientation) {
            ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
            ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
            ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(270f)
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
            ExifInterface.ORIENTATION_TRANSPOSE -> {
                matrix.postRotate(90f)
                matrix.postScale(-1f, 1f)
            }
            ExifInterface.ORIENTATION_TRANSVERSE -> {
                matrix.postRotate(270f)
                matrix.postScale(-1f, 1f)
            }
            else -> return bitmap
        }

        return runCatching {
            val rotated = Bitmap.createBitmap(
                bitmap,
                0,
                0,
                bitmap.width,
                bitmap.height,
                matrix,
                true
            )
            if (rotated != bitmap) {
                bitmap.recycle()
            }
            rotated
        }.getOrDefault(bitmap)
    }
}
