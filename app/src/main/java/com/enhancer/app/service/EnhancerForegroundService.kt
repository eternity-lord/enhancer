package com.enhancer.app.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.enhancer.app.EnhancerApplication
import com.enhancer.app.MainActivity
import com.enhancer.app.R
import com.enhancer.app.model.QueueErrorKind
import com.enhancer.app.model.QueueItem
import com.enhancer.app.model.QueueItemStatus
import com.enhancer.app.nativebridge.NativeEnhancer
import com.enhancer.app.nativebridge.NativeStatusCode
import com.enhancer.app.util.ImageHelper
import com.enhancer.app.util.MediaStoreSaver
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class EnhancerForegroundService : Service() {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var workerJob: Job? = null
    private var wakeLock: PowerManager.WakeLock? = null

    @Volatile
    private var forceOverrideThermalOnce: Boolean = false

    private val appInstance: EnhancerApplication
        get() = application as EnhancerApplication

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
        appInstance.thermalMonitor.startListening()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_CANCEL_ALL -> {
                appInstance.queueRepository.cancelAll()
                releaseWakeLockSafely()
                appInstance.queueRepository.setServiceRunning(false)
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_RESUME_THERMAL -> {
                forceOverrideThermalOnce = true
                appInstance.queueRepository.setThermalPaused(false)
            }
        }

        val initialBackend = if (appInstance.queueRepository.state.value.settings.safeMode) "CPU" else "Vulkan"
        val initialNotification = buildRunningNotification(
            currentIndex = 1,
            totalCount = appInstance.queueRepository.state.value.items.size.coerceAtLeast(1),
            progress = 0,
            fileName = getString(R.string.app_name),
            backend = initialBackend
        )
        promoteToForeground(initialNotification)

        if (workerJob?.isActive != true) {
            appInstance.queueRepository.prepareQueueRun()
            appInstance.queueRepository.setServiceRunning(true)
            workerJob = serviceScope.launch {
                processQueueLoop()
            }
        }

        return START_NOT_STICKY
    }

    override fun onDestroy() {
        releaseWakeLockSafely()
        appInstance.thermalMonitor.stopListening()
        appInstance.queueRepository.setServiceRunning(false)
        serviceScope.cancel()
        super.onDestroy()
    }

    private suspend fun processQueueLoop() {
        val repo = appInstance.queueRepository
        val thermal = appInstance.thermalMonitor

        try {
            while (serviceScope.isActive && !repo.isCancelAllActive()) {
                val hasWaiting = repo.state.value.items.any { it.status == QueueItemStatus.WAITING }
                if (!hasWaiting) {
                    break
                }

                // Thermal safety check before starting next queued item
                val thermalSnapshot = thermal.refreshNow()
                if (thermalSnapshot.isSevere && !forceOverrideThermalOnce) {
                    repo.setThermalPaused(true, thermalSnapshot.statusLabelRes)
                    releaseWakeLockSafely()
                    updateNotification(buildThermalPausedNotification())

                    // Poll until device cools down below SEVERE or user explicitly resumes/cancels
                    while (serviceScope.isActive && !repo.isCancelAllActive()) {
                        delay(2500L)
                        val latest = thermal.refreshNow()
                        if (!latest.isSevere || forceOverrideThermalOnce) {
                            repo.setThermalPaused(false)
                            break
                        }
                        if (repo.state.value.items.none { it.status == QueueItemStatus.WAITING }) {
                            break
                        }
                    }
                    if (repo.isCancelAllActive()) {
                        break
                    }
                }
                forceOverrideThermalOnce = false

                val item = repo.claimNextWaitingItem() ?: break
                acquireWakeLockForItem()
                processSingleItem(item)
            }
        } finally {
            releaseWakeLockSafely()
            repo.setThermalPaused(false)
            repo.setServiceRunning(false)
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun processSingleItem(item: QueueItem) {
        val repo = appInstance.queueRepository
        val settings = item.settingsOverride ?: repo.state.value.settings
        val startElapsedMs = SystemClock.elapsedRealtime()

        val allItems = repo.state.value.items
        val activeTotal = allItems.count { it.status != QueueItemStatus.CANCELLED }.coerceAtLeast(1)
        val doneBefore = allItems.count { it.status == QueueItemStatus.DONE }
        val currentHumanIndex = (doneBefore + 1).coerceAtMost(activeTotal)

        NativeEnhancer.resetCancelFlag()
        if (repo.isItemCancelled(item.id)) {
            repo.markItemCancelled(item.id)
            return
        }

        // 1. Decode source image with EXIF orientation applied on Kotlin side
        val orientedBitmap = ImageHelper.decodeOrientedSourceBitmap(
            context = applicationContext,
            uri = item.sourceUri,
            exifOrientation = item.exifOrientation
        )
        if (orientedBitmap == null) {
            repo.markItemFailed(
                itemId = item.id,
                errorKind = QueueErrorKind.DECODE_SOURCE,
                errorDetail = ""
            )
            return
        }

        if (repo.isItemCancelled(item.id)) {
            orientedBitmap.recycle()
            repo.markItemCancelled(item.id)
            return
        }

        // 2. Create scoped MediaStore entry in Pictures/Enhancer and detach raw POSIX fd
        val outputFileName = ImageHelper.buildOutputFileName(
            baseName = item.baseName,
            target = settings.target,
            model = settings.model,
            format = settings.format,
            settings = settings
        )
        val pendingOutput = MediaStoreSaver.createPendingOutput(
            context = applicationContext,
            fileName = outputFileName,
            format = settings.format
        )
        if (pendingOutput == null) {
            orientedBitmap.recycle()
            repo.markItemFailed(
                itemId = item.id,
                errorKind = QueueErrorKind.MEDIASTORE_CREATE,
                errorDetail = ""
            )
            return
        }

        var lastNotifMs = 0L

        val outcome = try {
            NativeEnhancer.processBitmapToFd(
                assetManager = applicationContext.assets,
                sourceBitmap = orientedBitmap,
                paramAssetPath = settings.model.paramAssetPath,
                binAssetPath = settings.model.binAssetPath,
                modelScale = settings.model.scale,
                targetLongSide = settings.target.targetLongSide,
                tileSize = settings.effectiveTileSize.nativeArg,
                disableFp16 = settings.disableFp16,
                maxQuality = settings.effectiveMaxQuality,
                ttaX8 = settings.effectiveTtaX8,
                safeMode = settings.safeMode,
                sharpness = settings.sharpness,
                outputFormatCode = settings.format.nativeCode,
                jpegQuality = settings.jpegQuality,
                detachedOutputFd = pendingOutput.detachedFd,
                onProgressUpdate = { pct, backend, tile ->
                    repo.updateRunningProgress(item.id, pct, backend, tile)
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastNotifMs >= 250L || pct == 100) {
                        lastNotifMs = now
                        updateNotification(
                            buildRunningNotification(
                                currentIndex = currentHumanIndex,
                                totalCount = activeTotal,
                                progress = pct,
                                fileName = item.displayName,
                                backend = backend
                            )
                        )
                    }
                }
            )
        } finally {
            if (!orientedBitmap.isRecycled) {
                orientedBitmap.recycle()
            }
        }

        when (outcome.status) {
            NativeStatusCode.OK -> {
                if (repo.isItemCancelled(item.id)) {
                    MediaStoreSaver.rollbackOutput(applicationContext, pendingOutput.uri)
                    repo.markItemCancelled(item.id)
                } else {
                    MediaStoreSaver.commitPendingOutput(
                        context = applicationContext,
                        uri = pendingOutput.uri,
                        width = outcome.outWidth,
                        height = outcome.outHeight
                    )
                    val elapsedSec = ((SystemClock.elapsedRealtime() - startElapsedMs) / 1000f)
                        .coerceAtLeast(0.1f)
                    repo.markItemDone(
                        itemId = item.id,
                        outputUri = pendingOutput.uri,
                        outputFileName = pendingOutput.fileName,
                        outputWidth = outcome.outWidth,
                        outputHeight = outcome.outHeight,
                        outputMimeType = settings.format.mimeType,
                        backendUsed = outcome.backendUsed,
                        usedCpuFallback = outcome.usedCpuFallback,
                        finalTileSize = outcome.finalTileSize,
                        fallbackSummary = outcome.fallbackSummary,
                        nativeDiagnostic = outcome.errorDetail,
                        modelUsed = settings.model,
                        targetUsed = settings.target,
                        appliedSettings = settings,
                        elapsedSeconds = elapsedSec
                    )
                }
            }
            NativeStatusCode.CANCELLED -> {
                MediaStoreSaver.rollbackOutput(applicationContext, pendingOutput.uri)
                repo.markItemCancelled(item.id)
            }
            else -> {
                MediaStoreSaver.rollbackOutput(applicationContext, pendingOutput.uri)
                val kind = when (outcome.status) {
                    NativeStatusCode.ERR_OUT_OF_MEMORY -> QueueErrorKind.OUT_OF_MEMORY
                    NativeStatusCode.ERR_ASSET_LOAD -> QueueErrorKind.ASSET_LOAD
                    NativeStatusCode.ERR_BITMAP_LOCK -> QueueErrorKind.BITMAP_LOCK
                    NativeStatusCode.ERR_VULKAN_INIT -> QueueErrorKind.VULKAN_INIT
                    NativeStatusCode.ERR_VK_ALLOCATOR -> QueueErrorKind.VK_ALLOCATOR
                    NativeStatusCode.ERR_NCNN_LOAD_PARAM -> QueueErrorKind.NCNN_LOAD_PARAM
                    NativeStatusCode.ERR_NCNN_LOAD_MODEL -> QueueErrorKind.NCNN_LOAD_MODEL
                    NativeStatusCode.ERR_NCNN_INPUT -> QueueErrorKind.NCNN_INPUT
                    NativeStatusCode.ERR_NCNN_EXTRACT -> QueueErrorKind.NCNN_EXTRACT
                    NativeStatusCode.ERR_INFERENCE -> QueueErrorKind.INFERENCE
                    NativeStatusCode.ERR_ENCODE_WRITE -> QueueErrorKind.ENCODE_WRITE
                    else -> QueueErrorKind.UNKNOWN
                }
                repo.markItemFailed(
                    itemId = item.id,
                    errorKind = kind,
                    errorDetail = outcome.errorDetail,
                    backendUsed = outcome.backendUsed,
                    nativeStatusCode = outcome.rawStatusCode,
                    fallbackSummary = outcome.fallbackSummary
                )
            }
        }
    }

    private fun acquireWakeLockForItem() {
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        val lock = wakeLock ?: pm.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "Enhancer::UpscaleQueueWakeLock"
        ).apply {
            setReferenceCounted(false)
            wakeLock = this
        }
        runCatching {
            // 45-minute safety cap per item (refreshed per queued photo)
            lock.acquire(45L * 60L * 1000L)
        }
    }

    private fun releaseWakeLockSafely() {
        runCatching {
            wakeLock?.let {
                if (it.isHeld) {
                    it.release()
                }
            }
        }
    }

    private fun promoteToForeground(notification: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification(notification: Notification) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
        runCatching {
            nm.notify(NOTIFICATION_ID, notification)
        }
    }

    private fun ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
            val channel = NotificationChannel(
                CHANNEL_ID,
                getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = getString(R.string.notif_channel_desc)
                setShowBadge(false)
            }
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildRunningNotification(
        currentIndex: Int,
        totalCount: Int,
        progress: Int,
        fileName: String,
        backend: String
    ): Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val contentPending = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val cancelIntent = Intent(this, EnhancerForegroundService::class.java).apply {
            action = ACTION_CANCEL_ALL
        }
        val cancelPending = PendingIntent.getService(
            this,
            1,
            cancelIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(
                getString(
                    R.string.notif_running_title,
                    currentIndex,
                    totalCount,
                    progress.coerceIn(0, 100)
                )
            )
            .setContentText(
                getString(
                    R.string.notif_running_text,
                    fileName,
                    backend.ifBlank { "Vulkan" }
                )
            )
            .setProgress(100, progress.coerceIn(0, 100), progress <= 0)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(contentPending)
            .addAction(0, getString(R.string.notif_action_cancel), cancelPending)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .build()
    }

    private fun buildThermalPausedNotification(): Notification {
        val openIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val contentPending = PendingIntent.getActivity(
            this,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val cancelIntent = Intent(this, EnhancerForegroundService::class.java).apply {
            action = ACTION_CANCEL_ALL
        }
        val cancelPending = PendingIntent.getService(
            this,
            1,
            cancelIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.notif_thermal_title))
            .setContentText(getString(R.string.notif_thermal_text))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(contentPending)
            .addAction(0, getString(R.string.notif_action_cancel), cancelPending)
            .setCategory(NotificationCompat.CATEGORY_STATUS)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "enhancer_upscale_channel"
        private const val NOTIFICATION_ID = 4101

        const val ACTION_START_QUEUE = "com.enhancer.app.action.START_QUEUE"
        const val ACTION_CANCEL_ALL = "com.enhancer.app.action.CANCEL_ALL"
        const val ACTION_RESUME_THERMAL = "com.enhancer.app.action.RESUME_THERMAL"

        fun startQueue(context: Context) {
            val intent = Intent(context, EnhancerForegroundService::class.java).apply {
                action = ACTION_START_QUEUE
            }
            ContextCompat.startForegroundService(context, intent)
        }

        fun resumeFromThermalPause(context: Context) {
            val intent = Intent(context, EnhancerForegroundService::class.java).apply {
                action = ACTION_RESUME_THERMAL
            }
            ContextCompat.startForegroundService(context, intent)
        }
    }
}
