package com.enhancer.app.data

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import androidx.annotation.StringRes
import com.enhancer.app.R
import com.enhancer.app.model.AppLanguageMode
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
import com.enhancer.app.model.QueueErrorKind
import com.enhancer.app.model.QueueItem
import com.enhancer.app.model.QueueItemStatus
import com.enhancer.app.model.TargetPreset
import com.enhancer.app.model.TileSizeOption
import com.enhancer.app.model.UpscaleModel
import com.enhancer.app.nativebridge.NativeEnhancer
import com.enhancer.app.util.DeviceMemorySnapshot
import com.enhancer.app.util.ImageHelper
import com.enhancer.app.util.MemoryEstimator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import java.util.UUID

data class QueueUiState(
    val settings: EnhancerSettings = EnhancerSettings(),
    val items: List<QueueItem> = emptyList(),
    val isServiceRunning: Boolean = false,
    val isPausedThermal: Boolean = false,
    @StringRes val thermalStatusRes: Int = R.string.thermal_status_severe,
    val vulkanDeviceName: String = "",
    val deviceMemory: DeviceMemorySnapshot = DeviceMemorySnapshot(2048, 4096, 256, false)
)

class EnhancerQueueRepository(private val appContext: Context) {

    private val prefs = appContext.getSharedPreferences("enhancer_prefs", Context.MODE_PRIVATE)

    private val _state = MutableStateFlow(
        QueueUiState(
            settings = loadSettings(),
            vulkanDeviceName = NativeEnhancer.getVulkanDeviceName(),
            deviceMemory = MemoryEstimator.readDeviceMemory(appContext)
        )
    )
    val state: StateFlow<QueueUiState> = _state.asStateFlow()

    @Volatile
    private var currentlyRunningId: String? = null

    @Volatile
    private var cancelAllRequested: Boolean = false

    fun refreshDeviceMemory() {
        _state.update { it.copy(deviceMemory = MemoryEstimator.readDeviceMemory(appContext)) }
    }

    fun updateSettings(transform: (EnhancerSettings) -> EnhancerSettings) {
        _state.update { cur ->
            val updated = transform(cur.settings)
            saveSettings(updated)
            cur.copy(
                settings = updated,
                deviceMemory = MemoryEstimator.readDeviceMemory(appContext)
            )
        }
    }

    suspend fun enqueueUris(uris: List<Uri>) = withContext(Dispatchers.IO) {
        if (uris.isEmpty()) return@withContext
        val added = mutableListOf<QueueItem>()
        for (uri in uris) {
            val info = ImageHelper.inspectImage(appContext, uri) ?: continue
            added.add(
                QueueItem(
                    id = UUID.randomUUID().toString(),
                    sourceUri = uri,
                    displayName = info.displayName,
                    baseName = info.sanitizedBaseName,
                    sourceWidth = info.orientedWidth,
                    sourceHeight = info.orientedHeight,
                    exifOrientation = info.exifOrientation,
                    thumbnail = info.thumbnail,
                    status = QueueItemStatus.WAITING
                )
            )
        }
        if (added.isNotEmpty()) {
            _state.update { cur ->
                cur.copy(
                    items = cur.items + added,
                    deviceMemory = MemoryEstimator.readDeviceMemory(appContext)
                )
            }
        }
    }

    /**
     * Enqueues a fresh run of an existing item's ORIGINAL image (`sourceUri`) with custom settings
     * without re-picking or deleting the previously saved result.
     */
    fun enqueueReprocessItem(sourceItem: QueueItem, customSettings: EnhancerSettings): String {
        cancelAllRequested = false
        val newId = UUID.randomUUID().toString()
        val copiedThumb = sourceItem.thumbnail?.takeIf { !it.isRecycled }?.let { bmp ->
            runCatching { bmp.copy(Bitmap.Config.ARGB_8888, false) }.getOrNull()
        }
        val reprocessItem = QueueItem(
            id = newId,
            sourceUri = sourceItem.sourceUri,
            displayName = sourceItem.displayName,
            baseName = sourceItem.baseName,
            sourceWidth = sourceItem.sourceWidth,
            sourceHeight = sourceItem.sourceHeight,
            exifOrientation = sourceItem.exifOrientation,
            thumbnail = copiedThumb,
            settingsOverride = customSettings,
            status = QueueItemStatus.WAITING
        )
        _state.update { cur ->
            cur.copy(
                items = cur.items + reprocessItem,
                deviceMemory = MemoryEstimator.readDeviceMemory(appContext)
            )
        }
        return newId
    }

    fun cancelOrRemoveItem(itemId: String) {
        if (currentlyRunningId == itemId) {
            NativeEnhancer.requestCancel()
            _state.update { cur ->
                cur.copy(
                    items = cur.items.map { item ->
                        if (item.id == itemId && item.status == QueueItemStatus.RUNNING) {
                            item.copy(status = QueueItemStatus.CANCELLED)
                        } else {
                            item
                        }
                    }
                )
            }
            return
        }

        _state.update { cur ->
            val target = cur.items.firstOrNull { it.id == itemId } ?: return@update cur
            val updatedItems = if (target.status == QueueItemStatus.WAITING) {
                cur.items.map {
                    if (it.id == itemId) it.copy(status = QueueItemStatus.CANCELLED, progress = 0)
                    else it
                }
            } else {
                target.thumbnail?.let { bmp -> if (!bmp.isRecycled) bmp.recycle() }
                cur.items.filterNot { it.id == itemId }
            }
            cur.copy(items = updatedItems)
        }
    }

    fun removeItemCompletely(itemId: String) {
        if (currentlyRunningId == itemId) {
            cancelOrRemoveItem(itemId)
            return
        }
        _state.update { cur ->
            val removed = cur.items.firstOrNull { it.id == itemId }
            removed?.thumbnail?.let { bmp -> if (!bmp.isRecycled) bmp.recycle() }
            cur.copy(items = cur.items.filterNot { it.id == itemId })
        }
    }

    fun cancelAll() {
        cancelAllRequested = true
        NativeEnhancer.requestCancel()
        _state.update { cur ->
            cur.copy(
                isPausedThermal = false,
                items = cur.items.map { item ->
                    when (item.status) {
                        QueueItemStatus.WAITING, QueueItemStatus.RUNNING ->
                            item.copy(status = QueueItemStatus.CANCELLED, progress = 0)
                        else -> item
                    }
                }
            )
        }
    }

    fun retryItem(itemId: String) {
        cancelAllRequested = false
        _state.update { cur ->
            cur.copy(
                items = cur.items.map { item ->
                    if (item.id == itemId &&
                        (item.status == QueueItemStatus.FAILED || item.status == QueueItemStatus.CANCELLED)
                    ) {
                        item.copy(
                            status = QueueItemStatus.WAITING,
                            progress = 0,
                            errorKind = QueueErrorKind.NONE,
                            errorDetail = ""
                        )
                    } else {
                        item
                    }
                }
            )
        }
    }

    fun retryAllFailed() {
        cancelAllRequested = false
        _state.update { cur ->
            cur.copy(
                items = cur.items.map { item ->
                    if (item.status == QueueItemStatus.FAILED || item.status == QueueItemStatus.CANCELLED) {
                        item.copy(
                            status = QueueItemStatus.WAITING,
                            progress = 0,
                            errorKind = QueueErrorKind.NONE,
                            errorDetail = ""
                        )
                    } else {
                        item
                    }
                }
            )
        }
    }

    fun clearFinished() {
        _state.update { cur ->
            val (finished, keep) = cur.items.partition {
                it.status == QueueItemStatus.DONE || it.status == QueueItemStatus.CANCELLED
            }
            finished.forEach { item ->
                item.thumbnail?.let { bmp -> if (!bmp.isRecycled) bmp.recycle() }
            }
            cur.copy(items = keep)
        }
    }

    // --- Service coordination methods ---

    fun prepareQueueRun() {
        cancelAllRequested = false
    }

    fun isCancelAllActive(): Boolean = cancelAllRequested

    fun setServiceRunning(running: Boolean) {
        if (!running) {
            currentlyRunningId = null
        }
        _state.update { it.copy(isServiceRunning = running) }
    }

    fun setThermalPaused(paused: Boolean, @StringRes statusLabelRes: Int = R.string.thermal_status_severe) {
        _state.update {
            it.copy(
                isPausedThermal = paused,
                thermalStatusRes = statusLabelRes
            )
        }
    }

    fun claimNextWaitingItem(): QueueItem? {
        if (cancelAllRequested) return null
        var claimed: QueueItem? = null
        _state.update { cur ->
            val idx = cur.items.indexOfFirst { it.status == QueueItemStatus.WAITING }
            if (idx < 0) {
                return@update cur
            }
            val item = cur.items[idx].copy(
                status = QueueItemStatus.RUNNING,
                progress = 0,
                errorKind = QueueErrorKind.NONE,
                errorDetail = "",
                usedCpuFallback = false
            )
            claimed = item
            currentlyRunningId = item.id
            val updatedList = cur.items.toMutableList().also { it[idx] = item }
            cur.copy(items = updatedList)
        }
        return claimed
    }

    fun isItemCancelled(itemId: String): Boolean {
        if (cancelAllRequested) return true
        val item = _state.value.items.firstOrNull { it.id == itemId } ?: return true
        return item.status == QueueItemStatus.CANCELLED
    }

    fun updateRunningProgress(itemId: String, progress: Int, backend: String, tileSize: Int) {
        _state.update { cur ->
            cur.copy(
                items = cur.items.map { item ->
                    if (item.id == itemId && item.status == QueueItemStatus.RUNNING) {
                        item.copy(
                            progress = progress.coerceIn(0, 100),
                            activeBackend = backend,
                            activeTileSize = tileSize
                        )
                    } else {
                        item
                    }
                }
            )
        }
    }

    fun markItemDone(
        itemId: String,
        outputUri: Uri,
        outputFileName: String,
        outputWidth: Int,
        outputHeight: Int,
        outputMimeType: String,
        backendUsed: String,
        usedCpuFallback: Boolean,
        finalTileSize: Int,
        modelUsed: UpscaleModel,
        targetUsed: TargetPreset,
        appliedSettings: EnhancerSettings,
        elapsedSeconds: Float
    ) {
        if (currentlyRunningId == itemId) {
            currentlyRunningId = null
        }
        _state.update { cur ->
            cur.copy(
                deviceMemory = MemoryEstimator.readDeviceMemory(appContext),
                items = cur.items.map { item ->
                    if (item.id == itemId) {
                        item.copy(
                            status = QueueItemStatus.DONE,
                            progress = 100,
                            outputUri = outputUri,
                            outputFileName = outputFileName,
                            outputWidth = outputWidth,
                            outputHeight = outputHeight,
                            outputMimeType = outputMimeType,
                            activeBackend = backendUsed,
                            usedCpuFallback = usedCpuFallback,
                            activeTileSize = finalTileSize,
                            modelUsed = modelUsed,
                            targetUsed = targetUsed,
                            appliedSettings = appliedSettings,
                            elapsedSeconds = elapsedSeconds,
                            errorKind = QueueErrorKind.NONE,
                            errorDetail = ""
                        )
                    } else {
                        item
                    }
                }
            )
        }
    }

    fun markItemCancelled(itemId: String) {
        if (currentlyRunningId == itemId) {
            currentlyRunningId = null
        }
        _state.update { cur ->
            cur.copy(
                items = cur.items.map { item ->
                    if (item.id == itemId) {
                        item.copy(status = QueueItemStatus.CANCELLED, progress = 0)
                    } else {
                        item
                    }
                }
            )
        }
    }

    fun markItemFailed(
        itemId: String,
        errorKind: QueueErrorKind,
        errorDetail: String,
        backendUsed: String = ""
    ) {
        if (currentlyRunningId == itemId) {
            currentlyRunningId = null
        }
        _state.update { cur ->
            cur.copy(
                items = cur.items.map { item ->
                    if (item.id == itemId) {
                        item.copy(
                            status = QueueItemStatus.FAILED,
                            progress = 0,
                            activeBackend = backendUsed.ifBlank { item.activeBackend },
                            errorKind = errorKind,
                            errorDetail = errorDetail
                        )
                    } else {
                        item
                    }
                }
            )
        }
    }

    private fun loadSettings(): EnhancerSettings {
        val model = runCatching {
            UpscaleModel.valueOf(prefs.getString("model", UpscaleModel.PHOTO_X4.name)!!)
        }.getOrDefault(UpscaleModel.PHOTO_X4)

        val target = runCatching {
            TargetPreset.valueOf(prefs.getString("target", TargetPreset.UHD_4K.name)!!)
        }.getOrDefault(TargetPreset.UHD_4K)

        val maxQuality = prefs.getBoolean("max_quality", false)
        val ttaX8 = prefs.getBoolean("tta_x8", false)
        val sharpness = prefs.getInt("sharpness", 0).coerceIn(0, 100)

        val format = runCatching {
            OutputFormat.valueOf(prefs.getString("format", OutputFormat.JPEG.name)!!)
        }.getOrDefault(OutputFormat.JPEG)

        val jpegQuality = prefs.getInt("jpeg_quality", 98).coerceIn(90, 100)

        val tile = runCatching {
            TileSizeOption.valueOf(prefs.getString("tile_size", TileSizeOption.AUTO.name)!!)
        }.getOrDefault(TileSizeOption.AUTO)

        val disableFp16 = prefs.getBoolean("disable_fp16", false)

        val lang = runCatching {
            AppLanguageMode.valueOf(prefs.getString("lang_mode", AppLanguageMode.SYSTEM.name)!!)
        }.getOrDefault(AppLanguageMode.SYSTEM)

        return EnhancerSettings(
            model = model,
            target = target,
            maxQuality = maxQuality,
            ttaX8 = ttaX8,
            sharpness = sharpness,
            format = format,
            jpegQuality = jpegQuality,
            tileSize = tile,
            disableFp16 = disableFp16,
            languageMode = lang
        )
    }

    private fun saveSettings(s: EnhancerSettings) {
        prefs.edit()
            .putString("model", s.model.name)
            .putString("target", s.target.name)
            .putBoolean("max_quality", s.maxQuality)
            .putBoolean("tta_x8", s.ttaX8)
            .putInt("sharpness", s.sharpness.coerceIn(0, 100))
            .putString("format", s.format.name)
            .putInt("jpeg_quality", s.jpegQuality.coerceIn(90, 100))
            .putString("tile_size", s.tileSize.name)
            .putBoolean("disable_fp16", s.disableFp16)
            .putString("lang_mode", s.languageMode.name)
            .apply()
    }
}
