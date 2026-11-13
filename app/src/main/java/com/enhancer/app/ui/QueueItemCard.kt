@file:OptIn(ExperimentalLayoutApi::class)

package com.enhancer.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Compare
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.enhancer.app.R
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.QueueErrorKind
import com.enhancer.app.model.QueueItem
import com.enhancer.app.model.QueueItemStatus
import com.enhancer.app.util.DeviceMemorySnapshot
import com.enhancer.app.util.MemoryEstimator

@Composable
fun QueueItemCard(
    item: QueueItem,
    settings: EnhancerSettings,
    deviceMemory: DeviceMemorySnapshot,
    onCancel: () -> Unit,
    onRemove: () -> Unit,
    onRetry: () -> Unit,
    onOpenCompare: () -> Unit,
    modifier: Modifier = Modifier
) {
    val activeSettings = item.appliedSettings ?: item.settingsOverride ?: settings
    val context = LocalContext.current
    val copiedToastText = stringResource(R.string.diag_copied_toast)
    var showErrorDialog by rememberSaveable { mutableStateOf(false) }

    val ramEstimate = remember(
        item.sourceWidth,
        item.sourceHeight,
        activeSettings,
        deviceMemory
    ) {
        MemoryEstimator.estimateForImage(
            srcWidth = item.sourceWidth,
            srcHeight = item.sourceHeight,
            settings = activeSettings,
            deviceMem = deviceMemory
        )
    }

    val plan = ramEstimate.plan
    val shownOutW = if (item.status == QueueItemStatus.DONE && item.outputWidth > 0) {
        item.outputWidth
    } else {
        plan.finalWidth
    }
    val shownOutH = if (item.status == QueueItemStatus.DONE && item.outputHeight > 0) {
        item.outputHeight
    } else {
        plan.finalHeight
    }

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        border = BorderStroke(
            width = 1.dp,
            color = when (item.status) {
                QueueItemStatus.RUNNING -> MaterialTheme.colorScheme.primary
                QueueItemStatus.DONE -> Color(0xFF10B981).copy(alpha = 0.6f)
                QueueItemStatus.FAILED -> MaterialTheme.colorScheme.error.copy(alpha = 0.7f)
                else -> MaterialTheme.colorScheme.surfaceVariant
            }
        )
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Thumbnail
                Box(
                    modifier = Modifier
                        .size(64.dp)
                        .clip(RoundedCornerShape(10.dp))
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                    contentAlignment = Alignment.Center
                ) {
                    val bmp = item.thumbnail
                    if (bmp != null && !bmp.isRecycled) {
                        Image(
                            bitmap = bmp.asImageBitmap(),
                            contentDescription = item.displayName,
                            contentScale = ContentScale.Crop,
                            modifier = Modifier.fillMaxSize()
                        )
                    } else {
                        Icon(
                            imageVector = Icons.Default.Image,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }

                Spacer(modifier = Modifier.width(12.dp))

                Column(modifier = Modifier.weight(1f)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = item.displayName,
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f)
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        StatusBadge(item = item)
                    }

                    Spacer(modifier = Modifier.height(4.dp))

                    Text(
                        text = stringResource(
                            R.string.res_arrow,
                            item.sourceWidth,
                            item.sourceHeight,
                            shownOutW,
                            shownOutH
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )

                    Spacer(modifier = Modifier.height(2.dp))

                    Text(
                        text = stringResource(R.string.est_ram_label, ramEstimate.estimatedPeakMb),
                        style = MaterialTheme.typography.labelMedium,
                        color = if (ramEstimate.isTooHighForDevice && item.status != QueueItemStatus.DONE) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.primary
                        }
                    )
                }

                if (item.status != QueueItemStatus.RUNNING) {
                    IconButton(onClick = onRemove) {
                        Icon(
                            imageVector = Icons.Default.DeleteOutline,
                            contentDescription = stringResource(R.string.action_remove),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }

            // Active v3.0 Studio AI mode & quality badges
            Spacer(modifier = Modifier.height(8.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                MiniFeatureBadge(stringResource(activeSettings.enhanceMode.labelRes))
                if (item.status == QueueItemStatus.DONE && item.facesRestored > 0) {
                    MiniFeatureBadge(stringResource(R.string.badge_faces_done, item.facesRestored))
                } else if (activeSettings.enhanceFaces) {
                    MiniFeatureBadge(stringResource(R.string.badge_faces))
                }
                if (activeSettings.lightAi) {
                    MiniFeatureBadge(stringResource(R.string.badge_light_ai))
                }
                MiniFeatureBadge(stringResource(R.string.badge_300_dpi))
                if (item.settingsOverride != null) {
                    MiniFeatureBadge(stringResource(R.string.badge_custom_settings))
                }
                if (activeSettings.usesCpuOnly) {
                    MiniFeatureBadge(stringResource(R.string.badge_safe_mode))
                }
                if (activeSettings.effectiveMaxQuality) {
                    MiniFeatureBadge(stringResource(R.string.badge_max_quality))
                }
                if (activeSettings.effectiveTtaX8) {
                    MiniFeatureBadge(stringResource(R.string.badge_tta_x8))
                }
                if (activeSettings.sharpness > 0) {
                    MiniFeatureBadge(stringResource(R.string.badge_sharpness, activeSettings.sharpness))
                }
            }

            // Pre-run / active warnings & resize notes
            if (item.status == QueueItemStatus.WAITING || item.status == QueueItemStatus.RUNNING) {
                if (ramEstimate.isTooHighForDevice) {
                    Spacer(modifier = Modifier.height(10.dp))
                    WarningInlineBox(
                        text = stringResource(
                            R.string.warn_high_ram,
                            ramEstimate.estimatedPeakMb,
                            deviceMemory.availMb
                        ),
                        isError = true
                    )
                }

                if (plan.preDownscaled) {
                    Spacer(modifier = Modifier.height(8.dp))
                    WarningInlineBox(
                        text = stringResource(
                            R.string.warn_large_source_limited_gain,
                            plan.workWidth,
                            plan.workHeight
                        )
                    )
                } else if (plan.postDownscaled) {
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(
                        text = stringResource(
                            R.string.info_max_quality_supersample,
                            plan.modelOutWidth,
                            plan.modelOutHeight,
                            plan.finalWidth,
                            plan.finalHeight
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary
                    )
                } else if (plan.postUpscaled) {
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(
                        text = stringResource(
                            R.string.info_post_lanczos,
                            plan.modelOutWidth,
                            plan.modelOutHeight,
                            plan.finalWidth,
                            plan.finalHeight
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            // Running progress bar + backend badge + Cancel button
            if (item.status == QueueItemStatus.RUNNING) {
                Spacer(modifier = Modifier.height(12.dp))
                LinearProgressIndicator(
                    progress = { (item.progress.coerceIn(0, 100)) / 100f },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(8.dp)
                        .clip(RoundedCornerShape(4.dp))
                )
                Spacer(modifier = Modifier.height(8.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = stringResource(
                            R.string.backend_tile_badge,
                            item.activeBackend.ifBlank { "Vulkan" },
                            item.activeTileSize
                        ),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary
                    )
                    OutlinedButton(onClick = onCancel) {
                        Icon(
                            imageVector = Icons.Default.Close,
                            contentDescription = null,
                            modifier = Modifier.size(16.dp)
                        )
                        Spacer(modifier = Modifier.width(4.dp))
                        Text(stringResource(R.string.action_cancel))
                    }
                }
            }

            // Done state: backend, elapsed time, exact fallback chain used, Compare & Share button
            if (item.status == QueueItemStatus.DONE) {
                Spacer(modifier = Modifier.height(10.dp))
                Text(
                    text = stringResource(
                        R.string.done_summary,
                        item.activeBackend.ifBlank { if (activeSettings.usesCpuOnly) "CPU" else "Vulkan" },
                        item.elapsedSeconds
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFF6EE7B7)
                )
                if (item.fallbackSummary.isNotBlank()) {
                    Spacer(modifier = Modifier.height(6.dp))
                    WarningInlineBox(
                        text = stringResource(R.string.fallback_chain_used, item.fallbackSummary)
                    )
                } else if (item.usedCpuFallback) {
                    Spacer(modifier = Modifier.height(6.dp))
                    WarningInlineBox(text = stringResource(R.string.fallback_notice))
                }
                Spacer(modifier = Modifier.height(10.dp))
                Button(
                    onClick = onOpenCompare,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Icon(
                        imageVector = Icons.Default.Compare,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(stringResource(R.string.action_compare))
                }
            }

            // Failed or Cancelled state: distinct reason + full native diagnostic dialog + Retry button
            if (item.status == QueueItemStatus.FAILED || item.status == QueueItemStatus.CANCELLED) {
                if (item.status == QueueItemStatus.FAILED) {
                    Spacer(modifier = Modifier.height(10.dp))
                    WarningInlineBox(
                        text = resolveErrorMessage(item),
                        isError = true
                    )
                }
                Spacer(modifier = Modifier.height(10.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    if (item.status == QueueItemStatus.FAILED && item.errorDetail.isNotBlank()) {
                        OutlinedButton(onClick = { showErrorDialog = true }) {
                            Icon(
                                imageVector = Icons.Default.BugReport,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp)
                            )
                            Spacer(modifier = Modifier.width(6.dp))
                            Text(stringResource(R.string.action_error_details))
                        }
                    }
                    FilledTonalButton(onClick = onRetry) {
                        Icon(
                            imageVector = Icons.Default.Refresh,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.action_retry))
                    }
                }
            }
        }
    }

    if (showErrorDialog && item.errorDetail.isNotBlank()) {
        AlertDialog(
            onDismissRequest = { showErrorDialog = false },
            title = {
                Text(
                    text = stringResource(R.string.error_dialog_title),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
            },
            text = {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(
                        text = resolveErrorMessage(item),
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.error
                    )
                    Surface(
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(10.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f)
                    ) {
                        SelectionContainer {
                            Text(
                                text = item.errorDetail,
                                style = MaterialTheme.typography.bodySmall,
                                fontFamily = FontFamily.Monospace,
                                color = MaterialTheme.colorScheme.onSurface,
                                modifier = Modifier.padding(10.dp)
                            )
                        }
                    }
                }
            },
            confirmButton = {
                Button(
                    onClick = {
                        copyToClipboard(context, item.errorDetail, copiedToastText)
                    }
                ) {
                    Icon(
                        imageVector = Icons.Default.ContentCopy,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(stringResource(R.string.action_copy_diag))
                }
            },
            dismissButton = {
                TextButton(onClick = { showErrorDialog = false }) {
                    Text(stringResource(R.string.action_close))
                }
            }
        )
    }
}

@Composable
private fun MiniFeatureBadge(text: String) {
    Surface(
        shape = RoundedCornerShape(6.dp),
        color = MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.7f)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Medium,
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp)
        )
    }
}

@Composable
private fun StatusBadge(item: QueueItem) {
    val (label, bg, fg) = when (item.status) {
        QueueItemStatus.WAITING -> Triple(
            stringResource(R.string.status_waiting),
            MaterialTheme.colorScheme.surfaceVariant,
            MaterialTheme.colorScheme.onSurfaceVariant
        )
        QueueItemStatus.RUNNING -> Triple(
            stringResource(R.string.status_running, item.progress),
            MaterialTheme.colorScheme.primaryContainer,
            MaterialTheme.colorScheme.onPrimaryContainer
        )
        QueueItemStatus.DONE -> Triple(
            stringResource(R.string.status_done),
            Color(0xFF065F46),
            Color(0xFFD1FAE5)
        )
        QueueItemStatus.FAILED -> Triple(
            stringResource(R.string.status_failed),
            MaterialTheme.colorScheme.errorContainer,
            MaterialTheme.colorScheme.onErrorContainer
        )
        QueueItemStatus.CANCELLED -> Triple(
            stringResource(R.string.status_cancelled),
            MaterialTheme.colorScheme.surfaceVariant,
            MaterialTheme.colorScheme.onSurfaceVariant
        )
    }

    Surface(
        shape = RoundedCornerShape(8.dp),
        color = bg
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = fg,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
        )
    }
}

@Composable
private fun resolveErrorMessage(item: QueueItem): String {
    val detail = item.errorDetail
    return when (item.errorKind) {
        QueueErrorKind.DECODE_SOURCE -> stringResource(R.string.err_decode_source)
        QueueErrorKind.MEDIASTORE_CREATE -> stringResource(R.string.err_mediastore_create)
        QueueErrorKind.OUT_OF_MEMORY -> {
            if (detail.isNotBlank()) stringResource(R.string.err_oom_with_detail, detail)
            else stringResource(R.string.err_oom)
        }
        QueueErrorKind.ASSET_LOAD -> {
            if (detail.isNotBlank()) stringResource(R.string.err_asset_load_detail, detail)
            else stringResource(R.string.err_asset_load)
        }
        QueueErrorKind.BITMAP_LOCK -> {
            if (detail.isNotBlank()) stringResource(R.string.err_bitmap_lock_detail, detail)
            else stringResource(R.string.err_bitmap_lock)
        }
        QueueErrorKind.VULKAN_INIT -> stringResource(
            R.string.err_vulkan_init,
            detail.ifBlank { "ERR_VULKAN_INIT" }
        )
        QueueErrorKind.VK_ALLOCATOR -> stringResource(
            R.string.err_vk_allocator,
            detail.ifBlank { "ERR_VK_ALLOCATOR" }
        )
        QueueErrorKind.NCNN_LOAD_PARAM -> stringResource(
            R.string.err_ncnn_load_param,
            detail.ifBlank { "ERR_NCNN_LOAD_PARAM" }
        )
        QueueErrorKind.NCNN_LOAD_MODEL -> stringResource(
            R.string.err_ncnn_load_model,
            detail.ifBlank { "ERR_NCNN_LOAD_MODEL" }
        )
        QueueErrorKind.NCNN_INPUT -> stringResource(
            R.string.err_ncnn_input,
            detail.ifBlank { "ERR_NCNN_INPUT" }
        )
        QueueErrorKind.NCNN_EXTRACT -> stringResource(
            R.string.err_ncnn_extract,
            detail.ifBlank { "ERR_NCNN_EXTRACT" }
        )
        QueueErrorKind.INFERENCE -> stringResource(
            R.string.err_inference,
            detail.ifBlank { "ERR_INFERENCE" }
        )
        QueueErrorKind.ENCODE_WRITE -> {
            if (detail.isNotBlank()) stringResource(R.string.err_encode_write_detail, detail)
            else stringResource(R.string.err_encode_write)
        }
        else -> stringResource(
            R.string.err_unknown,
            detail.ifBlank { "code=${item.nativeStatusCode}" }
        )
    }
}
