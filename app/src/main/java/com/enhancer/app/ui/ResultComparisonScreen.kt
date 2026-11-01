@file:OptIn(ExperimentalMaterial3Api::class)

package com.enhancer.app.ui

import android.content.Intent
import android.graphics.Bitmap
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroidSize
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.ZoomOutMap
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.exifinterface.media.ExifInterface
import com.enhancer.app.R
import com.enhancer.app.model.QueueItem
import com.enhancer.app.util.ImageHelper
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.roundToInt

@Composable
fun ResultComparisonScreen(
    item: QueueItem,
    onBack: () -> Unit
) {
    BackHandler(onBack = onBack)

    val context = LocalContext.current
    var beforeBitmap by remember(item.id) { mutableStateOf<Bitmap?>(null) }
    var afterBitmap by remember(item.id) { mutableStateOf<Bitmap?>(null) }
    var isLoading by remember(item.id) { mutableStateOf(true) }

    // Downscaled decode only (max 1600 px long side) — NEVER decodes the full 4K/8K bitmap!
    LaunchedEffect(item.id, item.outputUri) {
        isLoading = true
        val (before, after) = withContext(Dispatchers.IO) {
            val b = ImageHelper.decodeDownscaledBitmap(
                context = context,
                uri = item.sourceUri,
                maxLongSide = 1600,
                exifOrientation = item.exifOrientation
            )
            val a = item.outputUri?.let { outUri ->
                ImageHelper.decodeDownscaledBitmap(
                    context = context,
                    uri = outUri,
                    maxLongSide = 1600,
                    exifOrientation = ExifInterface.ORIENTATION_NORMAL
                )
            }
            b to a
        }
        beforeBitmap = before
        afterBitmap = after
        isLoading = false
    }

    DisposableEffect(item.id) {
        onDispose {
            beforeBitmap?.let { if (!it.isRecycled) it.recycle() }
            afterBitmap?.let { if (!it.isRecycled) it.recycle() }
        }
    }

    var sliderFraction by remember(item.id) { mutableFloatStateOf(0.5f) }
    var zoomScale by remember(item.id) { mutableFloatStateOf(1f) }
    var panOffset by remember(item.id) { mutableStateOf(Offset.Zero) }

    val errNoShare = stringResource(R.string.error_no_share_app)
    val errNoGallery = stringResource(R.string.error_no_gallery_app)
    val shareTitle = stringResource(R.string.share_chooser_title)

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            text = stringResource(R.string.result_title),
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.SemiBold
                        )
                        Text(
                            text = stringResource(
                                R.string.backend_ran_label,
                                item.activeBackend.ifBlank { "Vulkan" }
                            ),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary
                        )
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.back)
                        )
                    }
                },
                actions = {
                    if (zoomScale > 1.02f) {
                        AssistChip(
                            onClick = {
                                zoomScale = 1f
                                panOffset = Offset.Zero
                            },
                            label = { Text(stringResource(R.string.reset_zoom)) },
                            leadingIcon = {
                                Icon(
                                    imageVector = Icons.Default.ZoomOutMap,
                                    contentDescription = null,
                                    modifier = Modifier.size(16.dp)
                                )
                            },
                            modifier = Modifier.padding(end = 8.dp)
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        containerColor = MaterialTheme.colorScheme.background
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 16.dp, vertical = 8.dp)
        ) {
            // Interactive Before/After Comparison Viewport
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0xFF06090F)),
                contentAlignment = Alignment.Center
            ) {
                val bBmp = beforeBitmap
                val aBmp = afterBitmap

                if (isLoading) {
                    CircularProgressIndicator()
                } else if (bBmp != null && aBmp != null && !bBmp.isRecycled && !aBmp.isRecycled) {
                    val beforeImg = remember(bBmp) { bBmp.asImageBitmap() }
                    val afterImg = remember(aBmp) { aBmp.asImageBitmap() }

                    Canvas(
                        modifier = Modifier
                            .fillMaxSize()
                            .pointerInput(item.id) {
                                val handleTouchRadiusPx = 44.dp.toPx()
                                awaitEachGesture {
                                    val down = awaitFirstDown(requireUnconsumed = false)
                                    val currentSplitX = size.width * sliderFraction
                                    var draggingDivider =
                                        zoomScale <= 1.02f || abs(down.position.x - currentSplitX) <= handleTouchRadiusPx

                                    if (draggingDivider && size.width > 0) {
                                        sliderFraction = (down.position.x / size.width.toFloat())
                                            .coerceIn(0.02f, 0.98f)
                                    }

                                    do {
                                        val event = awaitPointerEvent()
                                        val activeCount = event.changes.count { it.pressed }
                                        if (activeCount >= 2) {
                                            draggingDivider = false
                                            val zoomChange = event.calculateZoom()
                                            val panChange = event.calculatePan()
                                            val centroidSize = event.calculateCentroidSize(useCurrent = false)
                                            if (centroidSize > 0f || zoomChange != 1f || panChange != Offset.Zero) {
                                                val newZoom = (zoomScale * zoomChange).coerceIn(1f, 6f)
                                                val maxPanX = (size.width * (newZoom - 1f)) / 2f
                                                val maxPanY = (size.height * (newZoom - 1f)) / 2f
                                                zoomScale = newZoom
                                                panOffset = if (newZoom <= 1.01f) {
                                                    Offset.Zero
                                                } else {
                                                    Offset(
                                                        x = (panOffset.x + panChange.x).coerceIn(-maxPanX, maxPanX),
                                                        y = (panOffset.y + panChange.y).coerceIn(-maxPanY, maxPanY)
                                                    )
                                                }
                                            }
                                            event.changes.forEach {
                                                if (it.positionChanged()) it.consume()
                                            }
                                        } else if (activeCount == 1) {
                                            val change = event.changes.first { it.pressed }
                                            if (draggingDivider && size.width > 0) {
                                                sliderFraction = (change.position.x / size.width.toFloat())
                                                    .coerceIn(0.02f, 0.98f)
                                                if (change.positionChanged()) change.consume()
                                            } else if (zoomScale > 1.01f) {
                                                val delta = change.position - change.previousPosition
                                                val maxPanX = (size.width * (zoomScale - 1f)) / 2f
                                                val maxPanY = (size.height * (zoomScale - 1f)) / 2f
                                                panOffset = Offset(
                                                    x = (panOffset.x + delta.x).coerceIn(-maxPanX, maxPanX),
                                                    y = (panOffset.y + delta.y).coerceIn(-maxPanY, maxPanY)
                                                )
                                                if (change.positionChanged()) change.consume()
                                            }
                                        }
                                    } while (event.changes.any { it.pressed })
                                }
                            }
                    ) {
                        val cw = size.width
                        val ch = size.height
                        if (cw <= 0f || ch <= 0f) return@Canvas

                        val imgAspect = afterImg.width.toFloat() / afterImg.height.toFloat().coerceAtLeast(1f)
                        val fitScale = min(cw / afterImg.width.toFloat(), ch / afterImg.height.toFloat())
                        val baseW = afterImg.width.toFloat() * fitScale
                        val baseH = baseW / imgAspect.coerceAtLeast(0.001f)

                        val drawW = (baseW * zoomScale).roundToInt().coerceAtLeast(1)
                        val drawH = (baseH * zoomScale).roundToInt().coerceAtLeast(1)
                        val dstX = ((cw - drawW) / 2f + panOffset.x).roundToInt()
                        val dstY = ((ch - drawH) / 2f + panOffset.y).roundToInt()
                        val dstOffset = IntOffset(dstX, dstY)
                        val dstSize = IntSize(drawW, drawH)

                        // Full layer: AFTER (Enhanced)
                        drawImage(
                            image = afterImg,
                            srcOffset = IntOffset.Zero,
                            srcSize = IntSize(afterImg.width, afterImg.height),
                            dstOffset = dstOffset,
                            dstSize = dstSize,
                            filterQuality = FilterQuality.High
                        )

                        // Clipped left side: BEFORE (Original)
                        val splitX = cw * sliderFraction
                        clipRect(left = 0f, top = 0f, right = splitX, bottom = ch) {
                            drawImage(
                                image = beforeImg,
                                srcOffset = IntOffset.Zero,
                                srcSize = IntSize(beforeImg.width, beforeImg.height),
                                dstOffset = dstOffset,
                                dstSize = dstSize,
                                filterQuality = FilterQuality.Low
                            )
                        }

                        // Vertical divider line + handle
                        drawLine(
                            color = Color.White,
                            start = Offset(splitX, 0f),
                            end = Offset(splitX, ch),
                            strokeWidth = 2.5.dp.toPx()
                        )

                        val centerHandle = Offset(splitX, ch / 2f)
                        drawCircle(
                            color = Color(0xCC0F172A),
                            radius = 18.dp.toPx(),
                            center = centerHandle
                        )
                        drawCircle(
                            color = Color.White,
                            radius = 16.dp.toPx(),
                            center = centerHandle,
                            style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.dp.toPx())
                        )

                        // Small left/right chevrons inside the handle
                        val r = 5.dp.toPx()
                        drawLine(
                            color = Color.White,
                            start = Offset(centerHandle.x - r, centerHandle.y),
                            end = Offset(centerHandle.x - 1.5.dp.toPx(), centerHandle.y - r * 0.8f),
                            strokeWidth = 1.8.dp.toPx()
                        )
                        drawLine(
                            color = Color.White,
                            start = Offset(centerHandle.x - r, centerHandle.y),
                            end = Offset(centerHandle.x - 1.5.dp.toPx(), centerHandle.y + r * 0.8f),
                            strokeWidth = 1.8.dp.toPx()
                        )
                        drawLine(
                            color = Color.White,
                            start = Offset(centerHandle.x + r, centerHandle.y),
                            end = Offset(centerHandle.x + 1.5.dp.toPx(), centerHandle.y - r * 0.8f),
                            strokeWidth = 1.8.dp.toPx()
                        )
                        drawLine(
                            color = Color.White,
                            start = Offset(centerHandle.x + r, centerHandle.y),
                            end = Offset(centerHandle.x + 1.5.dp.toPx(), centerHandle.y + r * 0.8f),
                            strokeWidth = 1.8.dp.toPx()
                        )
                    }

                    // Physical Left = BEFORE, Physical Right = AFTER badges
                    androidx.compose.runtime.CompositionLocalProvider(
                        androidx.compose.ui.platform.LocalLayoutDirection provides
                            androidx.compose.ui.unit.LayoutDirection.Ltr
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .align(Alignment.TopCenter)
                                .padding(12.dp),
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            OverlayTag(text = "${stringResource(R.string.label_before)} (${item.sourceWidth}×${item.sourceHeight})")
                            OverlayTag(text = "${stringResource(R.string.label_after)} (${item.outputWidth}×${item.outputHeight})")
                        }
                    }
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            if (item.usedCpuFallback) {
                WarningInlineBox(text = stringResource(R.string.fallback_notice))
                Spacer(modifier = Modifier.height(8.dp))
            }

            Text(
                text = stringResource(
                    R.string.preview_downscaled_note,
                    item.outputWidth,
                    item.outputHeight,
                    item.outputFileName
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(modifier = Modifier.height(12.dp))

            // Share & Open in Gallery buttons
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OutlinedButton(
                    onClick = {
                        val outUri = item.outputUri ?: return@OutlinedButton
                        val viewIntent = Intent(Intent.ACTION_VIEW).apply {
                            setDataAndType(outUri, item.outputMimeType)
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        if (!context.startExternalActivitySafely(viewIntent)) {
                            Toast.makeText(context, errNoGallery, Toast.LENGTH_SHORT).show()
                        }
                    },
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.OpenInNew,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(stringResource(R.string.btn_open_gallery))
                }

                Button(
                    onClick = {
                        val outUri = item.outputUri ?: return@Button
                        val sendIntent = Intent(Intent.ACTION_SEND).apply {
                            type = item.outputMimeType
                            putExtra(Intent.EXTRA_STREAM, outUri)
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        val chooser = Intent.createChooser(sendIntent, shareTitle)
                        if (!context.startExternalActivitySafely(chooser)) {
                            Toast.makeText(context, errNoShare, Toast.LENGTH_SHORT).show()
                        }
                    },
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(
                        imageVector = Icons.Default.Share,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(stringResource(R.string.btn_share))
                }
            }

            Spacer(modifier = Modifier.height(8.dp))
        }
    }
}

@Composable
private fun OverlayTag(text: String) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = Color(0xB30F172A)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = Color.White,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp)
        )
    }
}
