package com.enhancer.app.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect as ComposeRect
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.exifinterface.media.ExifInterface
import com.enhancer.app.nativebridge.NativeEnhancer
import com.enhancer.app.util.ImageHelper
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.LinkedHashMap
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

enum class TileLayer {
    AFTER,
    ORIGINAL
}

data class TileKey(
    val layer: TileLayer,
    val sampleSize: Int,
    val gridX: Int,
    val gridY: Int
)

class RenderTile(
    val key: TileKey,
    val imageRect: Rect, // Coordinates in full-res output pixel space (0..outW, 0..outH)
    val bitmap: Bitmap,
    val imageBitmap: ImageBitmap,
    val byteCount: Int,
    val loadedAtUptimeMs: Long
)

/**
 * Memory-safe full-resolution tiled deep-zoom controller backed by `BitmapRegionDecoder`.
 *
 * - Keeps a lightweight downsampled base layer (`max 1280 px`) for instant zero-blank rendering.
 * - Decodes only visible tiles at power-of-two `inSampleSize` (down to `1` = true 1:1 pixels)
 *   from the saved output file and EXIF-aware Original image on `Dispatchers.IO`.
 * - Upscales Original patches to exact output scale using native Catmull-Rom bicubic (`resizeBitmapBicubic`).
 * - Bounds tile memory to a 64 MB LRU cache with frame-safe delayed `Bitmap.recycle()` and OOM recovery.
 */
class TiledDeepZoomController(
    private val appContext: Context,
    private val scope: CoroutineScope,
    private val sourceUri: Uri,
    private val sourceExifOrientation: Int,
    private val outputUri: Uri
) {
    companion object {
        private const val BASE_MAX_LONG_SIDE = 1280
        private const val TILE_PIXEL_SIZE = 512
        private const val MAX_CACHE_BYTES = 64 * 1024 * 1024 // 64 MB LRU budget
        const val CROSSFADE_DURATION_MS = 160L
    }

    var isReady by mutableStateOf(false)
        private set

    var loadFailed by mutableStateOf(false)
        private set

    var outputWidth by mutableStateOf(0)
        private set

    var outputHeight by mutableStateOf(0)
        private set

    var sourceWidth by mutableStateOf(0)
        private set

    var sourceHeight by mutableStateOf(0)
        private set

    var baseAfterImage by mutableStateOf<ImageBitmap?>(null)
        private set

    var baseOriginalImage by mutableStateOf<ImageBitmap?>(null)
        private set

    var activeAfterTiles by mutableStateOf<List<RenderTile>>(emptyList())
        private set

    var activeOriginalTiles by mutableStateOf<List<RenderTile>>(emptyList())
        private set

    // Incremented when tiles finish crossfading or arrive
    var tileGeneration by mutableLongStateOf(0L)
        private set

    private var baseAfterBitmap: Bitmap? = null
    private var baseOriginalBitmap: Bitmap? = null

    private var outPfd: ParcelFileDescriptor? = null
    private var outDecoder: BitmapRegionDecoder? = null
    private val outDecoderLock = Any()

    private var srcPfd: ParcelFileDescriptor? = null
    private var srcDecoder: BitmapRegionDecoder? = null
    private val srcDecoderLock = Any()
    private var srcRawWidth: Int = 0
    private var srcRawHeight: Int = 0

    private val mainHandler = Handler(Looper.getMainLooper())
    private val cacheLock = Any()
    private var currentCacheBytes: Int = 0
    private val lruCache = object : LinkedHashMap<TileKey, RenderTile>(64, 0.75f, true) {}
    private val retiredBitmaps = ArrayList<Bitmap>()

    private var viewportJob: Job? = null
    private var disposed = false

    suspend fun initialize() {
        withContext(Dispatchers.IO) {
            try {
                val openedOutPfd = appContext.contentResolver.openFileDescriptor(outputUri, "r")
                val openedSrcPfd = appContext.contentResolver.openFileDescriptor(sourceUri, "r")
                if (openedOutPfd == null || openedSrcPfd == null) {
                    openedOutPfd?.close()
                    openedSrcPfd?.close()
                    withContext(Dispatchers.Main) { loadFailed = true }
                    return@withContext
                }

                val oDec = createRegionDecoder(openedOutPfd)
                val sDec = createRegionDecoder(openedSrcPfd)
                if (oDec == null || sDec == null || oDec.width <= 0 || oDec.height <= 0 || sDec.width <= 0 || sDec.height <= 0) {
                    oDec?.recycle()
                    sDec?.recycle()
                    openedOutPfd.close()
                    openedSrcPfd.close()
                    withContext(Dispatchers.Main) { loadFailed = true }
                    return@withContext
                }

                outPfd = openedOutPfd
                outDecoder = oDec
                srcPfd = openedSrcPfd
                srcDecoder = sDec
                srcRawWidth = sDec.width
                srcRawHeight = sDec.height

                val swaps = ImageHelper.isQuarterTurnOrientation(sourceExifOrientation)
                val sOrientedW = if (swaps) srcRawHeight else srcRawWidth
                val sOrientedH = if (swaps) srcRawWidth else srcRawHeight
                val oW = oDec.width
                val oH = oDec.height

                // Decode lightweight low-res base layers for instant display
                val afterBase = ImageHelper.decodeDownscaledBitmap(
                    context = appContext,
                    uri = outputUri,
                    maxLongSide = BASE_MAX_LONG_SIDE,
                    exifOrientation = ExifInterface.ORIENTATION_NORMAL
                )
                val rawOrigBase = ImageHelper.decodeDownscaledBitmap(
                    context = appContext,
                    uri = sourceUri,
                    maxLongSide = BASE_MAX_LONG_SIDE,
                    exifOrientation = sourceExifOrientation
                )

                // Upscale base Original with native Catmull-Rom bicubic to match baseAfter dimensions
                val bicubicOrigBase = if (rawOrigBase != null && afterBase != null) {
                    val targetW = afterBase.width.coerceAtLeast(1)
                    val targetH = afterBase.height.coerceAtLeast(1)
                    if (rawOrigBase.width == targetW && rawOrigBase.height == targetH) {
                        rawOrigBase
                    } else {
                        val dst = try {
                            Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888)
                        } catch (_: OutOfMemoryError) {
                            null
                        }
                        if (dst != null && NativeEnhancer.resizeBitmapBicubic(
                                rawOrigBase,
                                0.0,
                                0.0,
                                rawOrigBase.width.toDouble(),
                                rawOrigBase.height.toDouble(),
                                dst
                            )
                        ) {
                            rawOrigBase.recycle()
                            dst
                        } else {
                            dst?.recycle()
                            rawOrigBase
                        }
                    }
                } else {
                    rawOrigBase
                }

                withContext(Dispatchers.Main) {
                    if (disposed) {
                        afterBase?.recycle()
                        bicubicOrigBase?.recycle()
                        return@withContext
                    }
                    outputWidth = oW
                    outputHeight = oH
                    sourceWidth = sOrientedW
                    sourceHeight = sOrientedH
                    baseAfterBitmap = afterBase
                    baseOriginalBitmap = bicubicOrigBase
                    baseAfterImage = afterBase?.asImageBitmap()
                    baseOriginalImage = bicubicOrigBase?.asImageBitmap()
                    isReady = (afterBase != null && bicubicOrigBase != null)
                    loadFailed = !isReady
                }
            } catch (_: Throwable) {
                withContext(Dispatchers.Main) { loadFailed = true }
            }
        }
    }

    /**
     * Triggered whenever zoom, pan, or viewport dimensions change.
     * Computes the visible rectangle in output image coordinates, chooses the optimal power-of-two
     * `inSampleSize`, immediately publishes any already-cached tiles, and decodes missing tiles
     * on `Dispatchers.IO` ordered from viewport center outward.
     */
    fun updateVisibleViewport(
        visibleOutputRect: ComposeRect,
        scalePxPerOutputPx: Float
    ) {
        if (!isReady || disposed || outputWidth <= 0 || outputHeight <= 0) return

        val clampedLeft = visibleOutputRect.left.coerceIn(0f, outputWidth.toFloat())
        val clampedTop = visibleOutputRect.top.coerceIn(0f, outputHeight.toFloat())
        val clampedRight = visibleOutputRect.right.coerceIn(clampedLeft, outputWidth.toFloat())
        val clampedBottom = visibleOutputRect.bottom.coerceIn(clampedTop, outputHeight.toFloat())
        if (clampedRight - clampedLeft < 1f || clampedBottom - clampedTop < 1f) return

        // Base layer already covers very low zoom levels cleanly without extra tile overhead
        val baseScale = BASE_MAX_LONG_SIDE.toFloat() / max(outputWidth, outputHeight).toFloat()
        if (scalePxPerOutputPx <= baseScale * 1.05f) {
            viewportJob?.cancel()
            activeAfterTiles = emptyList()
            activeOriginalTiles = emptyList()
            return
        }

        val sampleSize = computeSampleSize(scalePxPerOutputPx)
        val tileSpanOutPx = TILE_PIXEL_SIZE * sampleSize

        val gx0 = max(0, floor(clampedLeft / tileSpanOutPx).toInt())
        val gy0 = max(0, floor(clampedTop / tileSpanOutPx).toInt())
        val gx1 = min(
            (outputWidth + tileSpanOutPx - 1) / tileSpanOutPx - 1,
            floor((clampedRight - 0.001f) / tileSpanOutPx).toInt()
        )
        val gy1 = min(
            (outputHeight + tileSpanOutPx - 1) / tileSpanOutPx - 1,
            floor((clampedBottom - 0.001f) / tileSpanOutPx).toInt()
        )
        if (gx1 < gx0 || gy1 < gy0) return

        val centerX = (clampedLeft + clampedRight) * 0.5f
        val centerY = (clampedTop + clampedBottom) * 0.5f

        data class NeededCoord(val gx: Int, val gy: Int, val dist: Float)

        val coords = ArrayList<NeededCoord>((gx1 - gx0 + 1) * (gy1 - gy0 + 1))
        for (gy in gy0..gy1) {
            for (gx in gx0..gx1) {
                val cx = (gx + 0.5f) * tileSpanOutPx
                val cy = (gy + 0.5f) * tileSpanOutPx
                coords.add(NeededCoord(gx, gy, hypot(cx - centerX, cy - centerY)))
            }
        }
        coords.sortBy { it.dist }

        // Publish any already-cached tiles immediately (including coarser fallback tiles underneath crossfading ones)
        publishVisibleFromCache(gx0, gy0, gx1, gy1, sampleSize)

        viewportJob?.cancel()
        viewportJob = scope.launch(Dispatchers.IO) {
            var anyDecoded = false
            for (coord in coords) {
                for (layer in listOf(TileLayer.AFTER, TileLayer.ORIGINAL)) {
                    ensureActive()
                    val key = TileKey(layer, sampleSize, coord.gx, coord.gy)
                    val alreadyCached = synchronized(cacheLock) { lruCache.containsKey(key) }
                    if (alreadyCached) continue

                    val decoded = decodeSingleTile(key, tileSpanOutPx)
                    ensureActive()
                    if (decoded != null) {
                        val evicted = ArrayList<Bitmap>()
                        synchronized(cacheLock) {
                            putIntoCacheLocked(decoded, evicted)
                        }
                        anyDecoded = true
                        withContext(Dispatchers.Main) {
                            if (!disposed) {
                                publishVisibleFromCache(gx0, gy0, gx1, gy1, sampleSize)
                                tileGeneration++
                            }
                            scheduleSafeRecycle(evicted)
                        }
                    }
                }
            }
            if (anyDecoded) {
                // Trigger final recomposition once crossfade window completes
                delay(CROSSFADE_DURATION_MS + 24L)
                withContext(Dispatchers.Main) {
                    if (!disposed) {
                        tileGeneration++
                    }
                }
            }
        }
    }

    private fun publishVisibleFromCache(
        gx0: Int,
        gy0: Int,
        gx1: Int,
        gy1: Int,
        sampleSize: Int
    ) {
        val afterList = ArrayList<RenderTile>()
        val origList = ArrayList<RenderTile>()
        synchronized(cacheLock) {
            // Also include 1 coarser level if present so crossfade transitions over sharp parent tiles
            val parentSample = sampleSize * 2
            if (parentSample <= 32) {
                val pgx0 = gx0 / 2
                val pgy0 = gy0 / 2
                val pgx1 = gx1 / 2
                val pgy1 = gy1 / 2
                for (pgy in pgy0..pgy1) {
                    for (pgx in pgx0..pgx1) {
                        lruCache[TileKey(TileLayer.AFTER, parentSample, pgx, pgy)]?.let { afterList.add(it) }
                        lruCache[TileKey(TileLayer.ORIGINAL, parentSample, pgx, pgy)]?.let { origList.add(it) }
                    }
                }
            }
            for (gy in gy0..gy1) {
                for (gx in gx0..gx1) {
                    lruCache[TileKey(TileLayer.AFTER, sampleSize, gx, gy)]?.let { afterList.add(it) }
                    lruCache[TileKey(TileLayer.ORIGINAL, sampleSize, gx, gy)]?.let { origList.add(it) }
                }
            }
        }
        activeAfterTiles = afterList
        activeOriginalTiles = origList
    }

    private fun decodeSingleTile(key: TileKey, tileSpanOutPx: Int): RenderTile? {
        val outX0 = key.gridX * tileSpanOutPx
        val outY0 = key.gridY * tileSpanOutPx
        val outX1 = min(outputWidth, outX0 + tileSpanOutPx)
        val outY1 = min(outputHeight, outY0 + tileSpanOutPx)
        if (outX1 <= outX0 || outY1 <= outY0) return null

        val outRect = Rect(outX0, outY0, outX1, outY1)
        val bmp = when (key.layer) {
            TileLayer.AFTER -> decodeAfterTileBitmap(outRect, key.sampleSize)
            TileLayer.ORIGINAL -> decodeOriginalBicubicTileBitmap(outRect, key.sampleSize)
        } ?: return null

        return RenderTile(
            key = key,
            imageRect = outRect,
            bitmap = bmp,
            imageBitmap = bmp.asImageBitmap(),
            byteCount = bmp.allocationByteCount,
            loadedAtUptimeMs = SystemClock.uptimeMillis()
        )
    }

    private fun decodeAfterTileBitmap(outRect: Rect, sampleSize: Int): Bitmap? {
        val decoder = outDecoder ?: return null
        val opts = BitmapFactory.Options().apply {
            inSampleSize = sampleSize
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }
        return try {
            synchronized(outDecoderLock) {
                if (disposed) null else decoder.decodeRegion(outRect, opts)
            }
        } catch (_: OutOfMemoryError) {
            handleOutOfMemory()
            null
        } catch (_: Throwable) {
            null
        }
    }

    /**
     * Decodes only the corresponding region of the ORIGINAL source image (accounting for EXIF
     * rotation/flip) with a 3-pixel margin and resamples it to the exact tile resolution using
     * native Catmull-Rom bicubic (`NativeEnhancer.resizeBitmapBicubic`).
     */
    private fun decodeOriginalBicubicTileBitmap(outRect: Rect, outSampleSize: Int): Bitmap? {
        val decoder = srcDecoder ?: return null
        if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
            return null
        }

        val dstW = max(1, (outRect.width() + outSampleSize - 1) / outSampleSize)
        val dstH = max(1, (outRect.height() + outSampleSize - 1) / outSampleSize)

        // Exact fractional ROI in oriented source coordinates
        val sx0 = outRect.left.toDouble() * sourceWidth.toDouble() / outputWidth.toDouble()
        val sy0 = outRect.top.toDouble() * sourceHeight.toDouble() / outputHeight.toDouble()
        val sx1 = outRect.right.toDouble() * sourceWidth.toDouble() / outputWidth.toDouble()
        val sy1 = outRect.bottom.toDouble() * sourceHeight.toDouble() / outputHeight.toDouble()

        // Choose power-of-two source sampleSize only if the source region itself is much larger than dstW/dstH
        val srcSpanX = max(1.0, sx1 - sx0)
        var srcSample = 1
        while (srcSample * 2 <= 16 && (srcSpanX / (srcSample * 2)) >= dstW * 1.25) {
            srcSample *= 2
        }

        val margin = 3 * srcSample
        val patchLeft = (floor(sx0).toInt() - margin).coerceIn(0, sourceWidth)
        val patchTop = (floor(sy0).toInt() - margin).coerceIn(0, sourceHeight)
        val patchRight = (ceil(sx1).toInt() + margin).coerceIn(patchLeft + 1, sourceWidth)
        val patchBottom = (ceil(sy1).toInt() + margin).coerceIn(patchTop + 1, sourceHeight)

        val orientedPatchRect = Rect(patchLeft, patchTop, patchRight, patchBottom)
        val rawPatchRect = ImageHelper.mapOrientedRectToRaw(
            orientedRect = orientedPatchRect,
            orientedW = sourceWidth,
            orientedH = sourceHeight,
            rawW = srcRawWidth,
            rawH = srcRawHeight,
            orientation = sourceExifOrientation
        )
        if (rawPatchRect.width() <= 0 || rawPatchRect.height() <= 0) return null

        val opts = BitmapFactory.Options().apply {
            inSampleSize = srcSample
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }

        return try {
            val rawDecoded = synchronized(srcDecoderLock) {
                if (disposed) null else decoder.decodeRegion(rawPatchRect, opts)
            } ?: return null

            val orientedPatch = ImageHelper.applyExifToRegionBitmap(rawDecoded, sourceExifOrientation)
            val argbPatch = if (orientedPatch.config == Bitmap.Config.ARGB_8888) {
                orientedPatch
            } else {
                val copy = orientedPatch.copy(Bitmap.Config.ARGB_8888, false)
                if (copy != orientedPatch) orientedPatch.recycle()
                copy
            }

            // Map [sx0..sx1] x [sy0..sy1] into `argbPatch` pixel space
            val scaleToPatchX = argbPatch.width.toDouble() / orientedPatchRect.width().toDouble()
            val scaleToPatchY = argbPatch.height.toDouble() / orientedPatchRect.height().toDouble()
            val localX0 = (sx0 - patchLeft) * scaleToPatchX
            val localY0 = (sy0 - patchTop) * scaleToPatchY
            val localX1 = (sx1 - patchLeft) * scaleToPatchX
            val localY1 = (sy1 - patchTop) * scaleToPatchY

            val dstBmp = Bitmap.createBitmap(dstW, dstH, Bitmap.Config.ARGB_8888)
            val ok = NativeEnhancer.resizeBitmapBicubic(
                srcBitmap = argbPatch,
                roiX0 = localX0,
                roiY0 = localY0,
                roiX1 = localX1,
                roiY1 = localY1,
                dstBitmap = dstBmp
            )
            argbPatch.recycle()
            if (ok) {
                dstBmp
            } else {
                dstBmp.recycle()
                null
            }
        } catch (_: OutOfMemoryError) {
            handleOutOfMemory()
            null
        } catch (_: CancellationException) {
            throw CancellationException()
        } catch (_: Throwable) {
            null
        }
    }

    private fun putIntoCacheLocked(tile: RenderTile, outEvicted: MutableList<Bitmap>) {
        val prev = lruCache.put(tile.key, tile)
        if (prev != null && prev.bitmap != tile.bitmap) {
            currentCacheBytes -= prev.byteCount
            outEvicted.add(prev.bitmap)
        }
        currentCacheBytes += tile.byteCount

        val iterator = lruCache.entries.iterator()
        while (currentCacheBytes > MAX_CACHE_BYTES && iterator.hasNext() && lruCache.size > 4) {
            val eldest = iterator.next().value
            if (eldest.key == tile.key) break
            currentCacheBytes -= eldest.byteCount
            outEvicted.add(eldest.bitmap)
            iterator.remove()
        }
    }

    private fun handleOutOfMemory() {
        val evicted = ArrayList<Bitmap>()
        synchronized(cacheLock) {
            val targetBytes = MAX_CACHE_BYTES / 3
            val iterator = lruCache.entries.iterator()
            while (currentCacheBytes > targetBytes && iterator.hasNext()) {
                val entry = iterator.next().value
                currentCacheBytes -= entry.byteCount
                evicted.add(entry.bitmap)
                iterator.remove()
            }
            retiredBitmaps.addAll(evicted)
        }
        mainHandler.postDelayed({ flushRetiredBitmaps() }, 96L)
        System.gc()
    }

    private fun scheduleSafeRecycle(evicted: List<Bitmap>) {
        if (evicted.isEmpty()) return
        synchronized(cacheLock) {
            retiredBitmaps.addAll(evicted)
        }
        // Delay recycling across 4+ UI frames so no in-flight HardwareUI RenderNode references the Bitmap
        mainHandler.postDelayed({ flushRetiredBitmaps() }, 96L)
    }

    private fun flushRetiredBitmaps() {
        val toRecycle = ArrayList<Bitmap>()
        synchronized(cacheLock) {
            // Never recycle a bitmap that is still referenced by active UI state lists
            val activeSet = HashSet<Bitmap>(activeAfterTiles.size + activeOriginalTiles.size)
            activeAfterTiles.forEach { activeSet.add(it.bitmap) }
            activeOriginalTiles.forEach { activeSet.add(it.bitmap) }

            val keep = ArrayList<Bitmap>()
            for (bmp in retiredBitmaps) {
                if (activeSet.contains(bmp)) {
                    keep.add(bmp)
                } else {
                    toRecycle.add(bmp)
                }
            }
            retiredBitmaps.clear()
            retiredBitmaps.addAll(keep)
        }
        for (bmp in toRecycle) {
            if (!bmp.isRecycled) {
                runCatching { bmp.recycle() }
            }
        }
    }

    private fun computeSampleSize(scalePxPerOutputPx: Float): Int {
        var sample = 1
        while (sample * 2 <= 32 && (1f / (sample * 2)) >= scalePxPerOutputPx * 0.85f) {
            sample *= 2
        }
        return sample
    }

    private fun createRegionDecoder(pfd: ParcelFileDescriptor): BitmapRegionDecoder? {
        return runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                BitmapRegionDecoder.newInstance(pfd)
            } else {
                @Suppress("DEPRECATION")
                BitmapRegionDecoder.newInstance(pfd.fileDescriptor, false)
            }
        }.getOrNull()
    }

    fun dispose() {
        disposed = true
        viewportJob?.cancel()
        activeAfterTiles = emptyList()
        activeOriginalTiles = emptyList()
        baseAfterImage = null
        baseOriginalImage = null

        val allBitmaps = ArrayList<Bitmap>()
        synchronized(cacheLock) {
            lruCache.values.forEach { allBitmaps.add(it.bitmap) }
            lruCache.clear()
            currentCacheBytes = 0
            allBitmaps.addAll(retiredBitmaps)
            retiredBitmaps.clear()
        }

        synchronized(outDecoderLock) {
            runCatching { outDecoder?.recycle() }
            outDecoder = null
            runCatching { outPfd?.close() }
            outPfd = null
        }

        synchronized(srcDecoderLock) {
            runCatching { srcDecoder?.recycle() }
            srcDecoder = null
            runCatching { srcPfd?.close() }
            srcPfd = null
        }

        baseAfterBitmap?.let { allBitmaps.add(it) }
        baseOriginalBitmap?.let { allBitmaps.add(it) }
        baseAfterBitmap = null
        baseOriginalBitmap = null

        mainHandler.postDelayed({
            for (bmp in allBitmaps) {
                if (!bmp.isRecycled) {
                    runCatching { bmp.recycle() }
                }
            }
        }, 96L)
    }
}
