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
        val plan = computeResolutionPlan(srcWidth, srcHeight, settings.model, settings.target)

        // Source ARGB_8888 bitmap while locked
        val srcBitmapBytes = plan.srcWidth.toLong() * plan.srcHeight.toLong() * 4L

        // Native RGB888 buffers
        val workRgbBytes = if (plan.preDownscaled) {
            plan.workWidth.toLong() * plan.workHeight.toLong() * 3L
        } else {
            0L
        }
        val modelOutRgbBytes = plan.modelOutWidth.toLong() * plan.modelOutHeight.toLong() * 3L
        val finalOutRgbBytes = plan.finalWidth.toLong() * plan.finalHeight.toLong() * 3L

        // Model weights + single-tile intermediate feature maps
        val tileSide = (settings.tileSize.effectivePixels + 20).toLong()
        val bytesPerElem = if (settings.disableFp16) 4L else 2L
        val tileWorkspaceBytes = tileSide * tileSide * 64L * bytesPerElem * 6L
        val modelWeightsBytes = settings.model.approxWeightsMb.toLong() * MB *
            (if (settings.disableFp16) 2L else 1L)

        // Peak during Stage A (tiling inference)
        val stageA = if (plan.preDownscaled) {
            max(
                srcBitmapBytes + workRgbBytes,
                workRgbBytes + modelOutRgbBytes + modelWeightsBytes + tileWorkspaceBytes
            )
        } else {
            srcBitmapBytes + modelOutRgbBytes + modelWeightsBytes + tileWorkspaceBytes
        }

        // Peak during Stage B (streaming Lanczos3 when source_long * m < target;
        // note srcBitmap and ncnn::Net are already released before allocating final_rgb!)
        val stageB = if (plan.postUpscaled) {
            modelOutRgbBytes + finalOutRgbBytes + (2L * MB)
        } else {
            0L
        }

        // Peak during Stage C (stb_image_write; PNG allocates an extra compression buffer)
        val pngExtraBytes = if (settings.format == OutputFormat.PNG) {
            finalOutRgbBytes + (16L * MB)
        } else {
            8L * MB
        }
        val stageC = finalOutRgbBytes + pngExtraBytes

        val peakBytes = max(stageA, max(stageB, stageC))
        val peakMb = ((peakBytes + MB - 1) / MB).toInt().coerceAtLeast(1)

        val usableAvailMb = (deviceMem.availMb - deviceMem.thresholdMb / 2).coerceAtLeast(128)
        val isTooHigh = deviceMem.isLowMemory ||
            peakMb > (usableAvailMb * 0.80).toInt() ||
            peakMb > (deviceMem.totalMb * 0.45).toInt()

        return ImageRamEstimate(
            plan = plan,
            estimatedPeakMb = peakMb,
            isTooHighForDevice = isTooHigh,
            deviceMemory = deviceMem
        )
    }
}
