@file:OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)

package com.enhancer.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AddPhotoAlternate
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Thermostat
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.enhancer.app.BuildConfig
import com.enhancer.app.R
import com.enhancer.app.data.QueueUiState
import com.enhancer.app.model.AppLanguageMode
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.QueueItemStatus

@Composable
fun MainScreen(
    uiState: QueueUiState,
    onPickPhotos: () -> Unit,
    onBrowseFiles: () -> Unit,
    onStartQueue: () -> Unit,
    onResumeThermal: () -> Unit,
    onCancelAll: () -> Unit,
    onRetryAllFailed: () -> Unit,
    onClearFinished: () -> Unit,
    onCancelItem: (String) -> Unit,
    onRemoveItem: (String) -> Unit,
    onRetryItem: (String) -> Unit,
    onOpenCompare: (String) -> Unit,
    onOpenAbout: () -> Unit,
    onSettingsChanged: (EnhancerSettings) -> Unit,
    onLanguageSelected: (AppLanguageMode) -> Unit
) {
    val waitingCount = uiState.items.count { it.status == QueueItemStatus.WAITING }
    val runningItem = uiState.items.firstOrNull { it.status == QueueItemStatus.RUNNING }
    val doneCount = uiState.items.count { it.status == QueueItemStatus.DONE }
    val failedOrCancelledCount = uiState.items.count {
        it.status == QueueItemStatus.FAILED || it.status == QueueItemStatus.CANCELLED
    }
    val activeQueueTotal = uiState.items.count { it.status != QueueItemStatus.CANCELLED }

    val overallFraction = if (activeQueueTotal <= 0) {
        0f
    } else {
        val partial = (runningItem?.progress ?: 0) / 100f
        ((doneCount.toFloat() + partial) / activeQueueTotal.toFloat()).coerceIn(0f, 1f)
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Top Header + Version 2.0 + Offline Badge + Info Icon + Language Toggle
            item {
                HeaderSection(
                    uiState = uiState,
                    onOpenAbout = onOpenAbout,
                    onLanguageSelected = onLanguageSelected
                )
            }

            // Severe Thermal Pause Banner
            if (uiState.isPausedThermal) {
                item {
                    ThermalWarningBanner(
                        statusLabel = stringResource(uiState.thermalStatusRes),
                        onResume = onResumeThermal
                    )
                }
            }

            // Pick Photos / Browse Files buttons
            item {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Button(
                        onClick = onPickPhotos,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(
                            imageVector = Icons.Default.AddPhotoAlternate,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.pick_photos))
                    }

                    FilledTonalButton(
                        onClick = onBrowseFiles,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(
                            imageVector = Icons.Default.FolderOpen,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.browse_files))
                    }
                }
            }

            // Studio Controls Card (7 AI Modes, Enhance Faces, Light AI, 2K/4K/8K, 300 DPI — never locked!)
            item {
                ControlsSection(
                    settings = uiState.settings,
                    onSettingsChanged = onSettingsChanged,
                    enabled = true
                )
            }

            // Queue Header + Overall Progress + Batch Actions
            if (uiState.items.isNotEmpty()) {
                item {
                    QueueSummaryBar(
                        totalItems = uiState.items.size,
                        doneCount = doneCount,
                        activeTotal = activeQueueTotal.coerceAtLeast(1),
                        overallFraction = overallFraction,
                        isRunning = uiState.isServiceRunning,
                        waitingCount = waitingCount,
                        failedOrCancelledCount = failedOrCancelledCount,
                        hasFinished = doneCount > 0 || failedOrCancelledCount > 0,
                        onStartQueue = onStartQueue,
                        onCancelAll = onCancelAll,
                        onRetryAllFailed = onRetryAllFailed,
                        onClearFinished = onClearFinished
                    )
                }

                items(
                    items = uiState.items,
                    key = { it.id }
                ) { queueItem ->
                    QueueItemCard(
                        item = queueItem,
                        settings = uiState.settings,
                        deviceMemory = uiState.deviceMemory,
                        onCancel = { onCancelItem(queueItem.id) },
                        onRemove = { onRemoveItem(queueItem.id) },
                        onRetry = { onRetryItem(queueItem.id) },
                        onOpenCompare = { onOpenCompare(queueItem.id) }
                    )
                }
            } else {
                item {
                    EmptyQueueCard()
                }
            }
        }
    }
}

@Composable
private fun HeaderSection(
    uiState: QueueUiState,
    onOpenAbout: () -> Unit,
    onLanguageSelected: (AppLanguageMode) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Default.AutoAwesome,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(28.dp)
            )
            Spacer(modifier = Modifier.width(10.dp))
            Column(modifier = Modifier.weight(1f)) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Text(
                        text = stringResource(R.string.app_name),
                        style = MaterialTheme.typography.headlineSmall,
                        fontWeight = FontWeight.Bold
                    )
                    Surface(
                        shape = RoundedCornerShape(6.dp),
                        color = MaterialTheme.colorScheme.secondaryContainer
                    ) {
                        Text(
                            text = stringResource(R.string.version_format, BuildConfig.VERSION_NAME),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSecondaryContainer,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                    Surface(
                        shape = RoundedCornerShape(6.dp),
                        color = MaterialTheme.colorScheme.tertiaryContainer
                    ) {
                        Text(
                            text = stringResource(R.string.offline_badge),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onTertiaryContainer,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                }
                Text(
                    text = stringResource(R.string.app_subtitle),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            IconButton(onClick = onOpenAbout) {
                Icon(
                    imageVector = Icons.Outlined.Info,
                    contentDescription = stringResource(R.string.about_content_desc),
                    tint = MaterialTheme.colorScheme.primary
                )
            }
        }

        Spacer(modifier = Modifier.height(10.dp))

        // Device GPU & RAM info + Language selector row
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                val gpuText = if (uiState.settings.safeMode) {
                    stringResource(
                        R.string.diag_safe_mode_status,
                        stringResource(R.string.diag_state_on)
                    )
                } else if (uiState.vulkanDeviceName.isNotBlank()) {
                    stringResource(R.string.gpu_detected, uiState.vulkanDeviceName)
                } else {
                    stringResource(R.string.gpu_unavailable_cpu)
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Default.Memory,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(15.dp)
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text(
                        text = gpuText,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                }
                Text(
                    text = stringResource(
                        R.string.device_ram_summary,
                        uiState.deviceMemory.availMb,
                        uiState.deviceMemory.totalMb
                    ),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                AppLanguageMode.entries.forEach { mode ->
                    val label = when (mode) {
                        AppLanguageMode.SYSTEM -> stringResource(R.string.lang_system)
                        AppLanguageMode.EN -> stringResource(R.string.lang_en)
                        AppLanguageMode.FA -> stringResource(R.string.lang_fa)
                    }
                    FilterChip(
                        selected = uiState.settings.languageMode == mode,
                        onClick = { onLanguageSelected(mode) },
                        label = {
                            Text(
                                text = label,
                                style = MaterialTheme.typography.labelSmall
                            )
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun ThermalWarningBanner(
    statusLabel: String,
    onResume: () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer
        )
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = Icons.Default.Thermostat,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onErrorContainer
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = stringResource(R.string.thermal_paused_title),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onErrorContainer
                )
            }
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = stringResource(R.string.thermal_paused_body, statusLabel),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onErrorContainer
            )
            Spacer(modifier = Modifier.height(10.dp))
            OutlinedButton(onClick = onResume) {
                Text(
                    text = stringResource(R.string.resume_queue),
                    color = MaterialTheme.colorScheme.onErrorContainer
                )
            }
        }
    }
}

@Composable
private fun QueueSummaryBar(
    totalItems: Int,
    doneCount: Int,
    activeTotal: Int,
    overallFraction: Float,
    isRunning: Boolean,
    waitingCount: Int,
    failedOrCancelledCount: Int,
    hasFinished: Boolean,
    onStartQueue: () -> Unit,
    onCancelAll: () -> Unit,
    onRetryAllFailed: () -> Unit,
    onClearFinished: () -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = stringResource(R.string.queue_header, totalItems),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
                if (hasFinished && !isRunning) {
                    TextButton(onClick = onClearFinished) {
                        Text(stringResource(R.string.clear_finished))
                    }
                }
            }

            Spacer(modifier = Modifier.height(6.dp))

            val overallPct = (overallFraction * 100f).toInt().coerceIn(0, 100)
            Text(
                text = stringResource(
                    R.string.overall_progress_label,
                    doneCount,
                    activeTotal,
                    overallPct
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(modifier = Modifier.height(6.dp))

            LinearProgressIndicator(
                progress = { overallFraction },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(8.dp)
                    .clip(RoundedCornerShape(4.dp))
            )

            Spacer(modifier = Modifier.height(12.dp))

            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                if (!isRunning && waitingCount > 0) {
                    Button(onClick = onStartQueue) {
                        Icon(
                            imageVector = Icons.Default.PlayArrow,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.start_queue))
                    }
                }

                if (isRunning) {
                    OutlinedButton(onClick = onCancelAll) {
                        Icon(
                            imageVector = Icons.Default.Close,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.cancel_all))
                    }
                }

                if (!isRunning && failedOrCancelledCount > 0) {
                    FilledTonalButton(onClick = onRetryAllFailed) {
                        Icon(
                            imageVector = Icons.Default.Refresh,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.retry_all_failed))
                    }
                }
            }
        }
    }
}

@Composable
private fun EmptyQueueCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.7f)
        ),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 24.dp, vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Icon(
                imageVector = Icons.Default.AddPhotoAlternate,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(44.dp)
            )
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                text = stringResource(R.string.empty_queue_title),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                textAlign = TextAlign.Center
            )
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = stringResource(R.string.empty_queue_body),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center
            )
        }
    }
}
