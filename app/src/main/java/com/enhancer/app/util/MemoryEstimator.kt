package com.enhancer.app.util

import android.app.ActivityManager
import android.content.Context
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.ResolutionPlan
import com.enhancer.app.model.computeResolutionPlan
import kotlin.math.max

data class DeviceMemorySnapshot(
    val availMb: Int,
    val totalMb: Int,
    val thresholdMb: Int,
    val isLowMemory: Boolean
)

data class ImageRamEstimate(
    val plan: ResolutionPlan,
    val estimatedPeakMb: Int,
    val isTooHighForDevice: Boolean,
    val deviceMemory: DeviceMemorySnapshot
)

object MemoryEstimator {

    private const val MB = 1024L * 1024L

    fun readDeviceMemory(context: Context): DeviceMemorySnapshot {
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        if (am == null) {
            return DeviceMemorySnapshot(
                availMb = 2048,
                totalMb = 4096,
                thresholdMb = 256,
                isLowMemory = false
            )
        }
        val info = ActivityManager.MemoryInfo()
        am.getMemoryInfo(info)
        return DeviceMemorySnapshot(
            availMb = (info.availMem / MB).toInt().coerceAtLeast(1),
            totalMb = (info.totalMem / MB).toInt().coerceAtLeast(1),
            thresholdMb = (info.threshold / MB).toInt().coerceAtLeast(0),
            isLowMemory = info.lowMemory
        )
    }

    fun estimateForImage(
        srcWidth: Int,
        srcHeight: Int,
        settings: EnhancerSettings,
        deviceMem: DeviceMemorySnapshot
    ): ImageRamEstimate {
        val effMaxQuality = settings.effectiveMaxQuality
        val effTilePx = settings.effectiveTileSize.effectivePixels

        val plan = computeResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            model = settings.model,
            target = settings.target,
            maxQuality = effMaxQuality
        )

        val m = settings.model.scale.toLong()
        // Source ARGB_8888 bitmap (unlocked + recycled immediately after pre-downscale when preDownscaled == true)
        val srcBitmapBytes = plan.srcWidth.toLong() * plan.srcHeight.toLong() * 4L

        val workRgbBytes = if (plan.preDownscaled) {
            plan.workWidth.toLong() * plan.workHeight.toLong() * 3L
        } else {
            0L
        }

        // Only ONE full-size output RGB888 buffer (`final_w * final_h * 3`) exists across both JPEG and PNG!
        val finalOutRgbBytes = plan.finalWidth.toLong() * plan.finalHeight.toLong() * 3L
        val bandOutHeight = (effTilePx.toLong() + (if (effMaxQuality) 16L else 0L)) * m
        val bandBytes = if (effMaxQuality) {
            // Fixed-point uint16_t band_accum_q8 + 32px uint16_t seam + RGB emit strip + Lanczos3 ring
            val q8Band = bandOutHeight * plan.modelOutWidth.toLong() * 3L * 2L
            val seamBytes = 32L * plan.modelOutWidth.toLong() * 3L * 2L
            val emitStrip = if (plan.postDownscaled || plan.postUpscaled) {
                bandOutHeight * plan.modelOutWidth.toLong() * 3L
            } else {
                0L
            }
            q8Band + seamBytes + emitStrip + (4L * MB)
        } else if (plan.postUpscaled) {
            bandOutHeight * plan.modelOutWidth.toLong() * 3L + (2L * MB)
        } else {
            0L
        }

        val padPx = if (effMaxQuality) 64L else 20L
        val tileSide = effTilePx.toLong() + padPx
        val bytesPerElem = if (settings.disableFp16) 4L else 2L
        val tileWorkspaceBytes = tileSide * tileSide * 64L * bytesPerElem * 6L +
            (tileSide * m * tileSide * m * 3L * 4L) // per-tile planar float RGB accumulator
        val modelWeightsBytes = settings.model.approxWeightsMb.toLong() * MB *
            (if (settings.disableFp16) 2L else 1L)

        // Peak during Stage A (band-streamed tiled inference)
        val stageA = if (plan.preDownscaled) {
            max(
                srcBitmapBytes + workRgbBytes,
                workRgbBytes + finalOutRgbBytes + bandBytes + modelWeightsBytes + tileWorkspaceBytes
            )
        } else {
            srcBitmapBytes + finalOutRgbBytes + bandBytes + modelWeightsBytes + tileWorkspaceBytes
        }

        // Peak during Stage B: both 4:4:4 JPEG and zlib PNG stream row-by-row from `final_rgb` with zero full-size copy
        val stageB = finalOutRgbBytes + (4L * MB)

        val peakBytes = max(stageA, stageB)
        val peakMb = ((peakBytes + MB - 1) / MB).toInt().coerceAtLeast(1)

        val usableAvailMb = (deviceMem.availMb - deviceMem.thresholdMb / 2).coerceAtLeast(128)
        val isTooHigh = deviceMem.isLowMemory ||
            peakMb > (usableAvailMb * 0.82).toInt() ||
            peakMb > (deviceMem.totalMb * 0.48).toInt()

        return ImageRamEstimate(
            plan = plan,
            estimatedPeakMb = peakMb,
            isTooHighForDevice = isTooHigh,
            deviceMemory = deviceMem
        )
    }
}
