package com.enhancer.app.util

import android.app.ActivityManager
import android.content.Context
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
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
        val plan = computeResolutionPlan(
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            model = settings.model,
            target = settings.target,
            maxQuality = settings.maxQuality
        )

        val m = settings.model.scale
        // Source ARGB_8888 bitmap (unlocked immediately after pre-downscale when preDownscaled == true)
        val srcBitmapBytes = plan.srcWidth.toLong() * plan.srcHeight.toLong() * 4L

        val workRgbBytes = if (plan.preDownscaled) {
            plan.workWidth.toLong() * plan.workHeight.toLong() * 3L
        } else {
            0L
        }

        // v2.0 allocates ONLY `finalOutRgbBytes` plus a single tile-row band buffer (never full modelOut!)
        val finalOutRgbBytes = plan.finalWidth.toLong() * plan.finalHeight.toLong() * 3L
        val bandOutHeight = ((settings.tileSize.effectivePixels + (if (settings.maxQuality) 16 else 0)) * m).toLong()
        val bandBytes = if (settings.maxQuality) {
            // Float band_accum + 32px seam float buffer + RGB emit strip + Lanczos3 ring
            val floatBand = bandOutHeight * plan.modelOutWidth.toLong() * 3L * 4L
            val seamBytes = 32L * plan.modelOutWidth.toLong() * 3L * 4L
            val emitStrip = if (plan.postDownscaled || plan.postUpscaled) {
                bandOutHeight * plan.modelOutWidth.toLong() * 3L
            } else {
                0L
            }
            floatBand + seamBytes + emitStrip + (4L * MB)
        } else if (plan.postUpscaled) {
            bandOutHeight * plan.modelOutWidth.toLong() * 3L + (2L * MB)
        } else {
            0L
        }

        val padPx = if (settings.maxQuality) 64L else 20L
        val tileSide = settings.tileSize.effectivePixels.toLong() + padPx
        val bytesPerElem = if (settings.disableFp16) 4L else 2L
        val tileWorkspaceBytes = tileSide * tileSide * 64L * bytesPerElem * 6L +
            (tileSide * m * tileSide * m * 3L * 4L) // planar float RGB tile accumulator
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

        // Peak during Stage B (final encoding: JPEG 4:4:4 streams directly; PNG allocates compression buffer)
        val encodeExtraBytes = if (settings.format == OutputFormat.PNG) {
            finalOutRgbBytes + (16L * MB)
        } else {
            8L * MB
        }
        val stageB = finalOutRgbBytes + encodeExtraBytes

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
