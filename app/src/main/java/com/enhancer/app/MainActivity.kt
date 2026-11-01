package com.enhancer.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import com.enhancer.app.model.QueueItemStatus
import com.enhancer.app.service.EnhancerForegroundService
import com.enhancer.app.ui.AboutScreen
import com.enhancer.app.ui.LocalizedAppContainer
import com.enhancer.app.ui.MainScreen
import com.enhancer.app.ui.ResultComparisonScreen
import com.enhancer.app.ui.theme.EnhancerTheme
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private val appInstance: EnhancerApplication
        get() = application as EnhancerApplication

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            val repo = appInstance.queueRepository
            val uiState by repo.state.collectAsState()
            val coroutineScope = rememberCoroutineScope()

            var selectedResultItemId by rememberSaveable { mutableStateOf<String?>(null) }
            var showAboutScreen by rememberSaveable { mutableStateOf(false) }

            // Android 13+ POST_NOTIFICATIONS launcher before starting Foreground Service
            val notifPermissionLauncher = rememberLauncherForActivityResult(
                contract = ActivityResultContracts.RequestPermission()
            ) { _ ->
                // Start foreground service regardless of notification grant so offline upscaling works
                EnhancerForegroundService.startQueue(this@MainActivity)
            }

            fun triggerStartQueue() {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    val granted = ContextCompat.checkSelfPermission(
                        this@MainActivity,
                        Manifest.permission.POST_NOTIFICATIONS
                    ) == PackageManager.PERMISSION_GRANTED
                    if (!granted) {
                        notifPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                        return
                    }
                }
                EnhancerForegroundService.startQueue(this@MainActivity)
            }

            // 1. Photo Picker (single or multiple images)
            val photoPickerLauncher = rememberLauncherForActivityResult(
                contract = ActivityResultContracts.PickMultipleVisualMedia()
            ) { uris ->
                if (uris.isNotEmpty()) {
                    coroutineScope.launch {
                        repo.enqueueUris(uris)
                    }
                }
            }

            // 2. ACTION_OPEN_DOCUMENT with EXTRA_ALLOW_MULTIPLE = true
            val openDocumentLauncher = rememberLauncherForActivityResult(
                contract = ActivityResultContracts.OpenMultipleDocuments()
            ) { uris ->
                if (uris.isNotEmpty()) {
                    uris.forEach { uri ->
                        runCatching {
                            contentResolver.takePersistableUriPermission(
                                uri,
                                Intent.FLAG_GRANT_READ_URI_PERMISSION
                            )
                        }
                    }
                    coroutineScope.launch {
                        repo.enqueueUris(uris)
                    }
                }
            }

            EnhancerTheme(darkTheme = true) {
                LocalizedAppContainer(languageMode = uiState.settings.languageMode) {
                    val compareItem = selectedResultItemId?.let { id ->
                        uiState.items.firstOrNull {
                            it.id == id && it.status == QueueItemStatus.DONE && it.outputUri != null
                        }
                    }

                    when {
                        showAboutScreen -> {
                            AboutScreen(
                                uiState = uiState,
                                onRunSelfTest = {
                                    coroutineScope.launch {
                                        repo.runDiagnosticsSelfTest()
                                    }
                                },
                                onBack = { showAboutScreen = false }
                            )
                        }
                        compareItem != null -> {
                            ResultComparisonScreen(
                                item = compareItem,
                                defaultSettings = uiState.settings,
                                deviceMemory = uiState.deviceMemory,
                                onBack = { selectedResultItemId = null },
                                onReprocessWithSettings = { customSettings ->
                                    repo.enqueueReprocessItem(compareItem, customSettings)
                                    selectedResultItemId = null
                                    triggerStartQueue()
                                }
                            )
                        }
                        else -> {
                            MainScreen(
                                uiState = uiState,
                                onPickPhotos = {
                                    photoPickerLauncher.launch(
                                        PickVisualMediaRequest(
                                            ActivityResultContracts.PickVisualMedia.ImageOnly
                                        )
                                    )
                                },
                                onBrowseFiles = {
                                    openDocumentLauncher.launch(arrayOf("image/*"))
                                },
                                onStartQueue = { triggerStartQueue() },
                                onResumeThermal = {
                                    EnhancerForegroundService.resumeFromThermalPause(this@MainActivity)
                                },
                                onCancelAll = { repo.cancelAll() },
                                onRetryAllFailed = {
                                    repo.retryAllFailed()
                                    triggerStartQueue()
                                },
                                onClearFinished = { repo.clearFinished() },
                                onCancelItem = { id -> repo.cancelOrRemoveItem(id) },
                                onRemoveItem = { id -> repo.removeItemCompletely(id) },
                                onRetryItem = { id ->
                                    repo.retryItem(id)
                                    if (!uiState.isServiceRunning) {
                                        triggerStartQueue()
                                    }
                                },
                                onOpenCompare = { id -> selectedResultItemId = id },
                                onOpenAbout = { showAboutScreen = true },
                                onModelSelected = { model ->
                                    repo.updateSettings { it.copy(model = model) }
                                },
                                onTargetSelected = { target ->
                                    repo.updateSettings { it.copy(target = target) }
                                },
                                onMaxQualityChanged = { maxQuality ->
                                    repo.updateSettings { it.copy(maxQuality = maxQuality) }
                                },
                                onTtaX8Changed = { tta ->
                                    repo.updateSettings { it.copy(ttaX8 = tta) }
                                },
                                onSafeModeChanged = { safe ->
                                    repo.updateSettings { it.copy(safeMode = safe) }
                                },
                                onSharpnessChanged = { sharp ->
                                    repo.updateSettings { it.copy(sharpness = sharp.coerceIn(0, 100)) }
                                },
                                onFormatSelected = { format ->
                                    repo.updateSettings { it.copy(format = format) }
                                },
                                onJpegQualityChanged = { quality ->
                                    repo.updateSettings { it.copy(jpegQuality = quality.coerceIn(90, 100)) }
                                },
                                onTileSizeSelected = { tile ->
                                    repo.updateSettings { it.copy(tileSize = tile) }
                                },
                                onDisableFp16Changed = { disable ->
                                    repo.updateSettings { it.copy(disableFp16 = disable) }
                                },
                                onLanguageSelected = { mode ->
                                    repo.updateSettings { it.copy(languageMode = mode) }
                                }
                            )
                        }
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        appInstance.queueRepository.refreshDeviceMemory()
    }
}
