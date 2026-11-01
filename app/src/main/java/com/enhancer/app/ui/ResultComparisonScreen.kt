@file:OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)

package com.enhancer.app.ui

import android.content.Intent
import android.os.SystemClock
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.AnimationState
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.animateDecay
import androidx.compose.animation.core.exponentialDecay
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculateCentroidSize
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
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
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import com.enhancer.app.R
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
import com.enhancer.app.model.QueueItem
import com.enhancer.app.model.TargetPreset
import com.enhancer.app.model.UpscaleModel
import com.enhancer.app.util.DeviceMemorySnapshot
import com.enhancer.app.util.MemoryEstimator
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

@Composable
fun ResultComparisonScreen(
    item: QueueItem,
    defaultSettings: EnhancerSettings,
    deviceMemory: DeviceMemorySnapshot,
    onBack: () -> Unit,
    onReprocessWithSettings: (EnhancerSettings) -> Unit
) {
    BackHandler(onBack = onBack)

    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val outputUri = item.outputUri

    val tileController = remember(item.id, outputUri) {
        outputUri?.let { outUri ->
            TiledDeepZoomController(
                appContext = context.applicationContext,
                scope = coroutineScope,
                sourceUri = item.sourceUri,
                sourceExifOrientation = item.exifOrientation,
                outputUri = outUri
            )
        }
    }

    LaunchedEffect(tileController) {
        tileController?.initialize()
    }

    DisposableEffect(tileController) {
        onDispose {
            tileController?.dispose()
        }
    }

    var viewportSize by remember(item.id) { mutableStateOf(IntSize.Zero) }
    var sliderFraction by remember(item.id) { mutableFloatStateOf(0.5f) }

    // Camera state in FULL-RES output pixel coordinates (0..outW, 0..outH):
    //   `scalePxPerImgPx == 1.0f` is exact 1:1 (100%); `4.0f` is 4x beyond 1:1 (400%).
    var scalePxPerImgPx by remember(item.id) { mutableFloatStateOf(0f) }
    var centerImgX by remember(item.id) { mutableFloatStateOf(0f) }
    var centerImgY by remember(item.id) { mutableFloatStateOf(0f) }
    var useNearestFilter by remember(item.id) { mutableStateOf(false) }
    var showReprocessSheet by remember(item.id) { mutableStateOf(false) }

    var cameraAnimJob by remember { mutableStateOf<Job?>(null) }

    val outW = (tileController?.outputWidth?.takeIf { it > 0 } ?: item.outputWidth).coerceAtLeast(1)
    val outH = (tileController?.outputHeight?.takeIf { it > 0 } ?: item.outputHeight).coerceAtLeast(1)

    val fitScale = remember(viewportSize, outW, outH) {
        if (viewportSize.width > 0 && viewportSize.height > 0) {
            min(
                viewportSize.width.toFloat() / outW.toFloat(),
                viewportSize.height.toFloat() / outH.toFloat()
            ).coerceAtLeast(0.01f)
        } else {
            0.25f
        }
    }
    // Guarantee zoom range from fit-screen up to at least 1:1 (1.0f) AND 4x beyond 1:1 (4.0f = 400%)
    val maxScale = remember(fitScale) { max(4.0f, fitScale * 8f) }

    fun clampCenterX(cx: Float, scale: Float, vw: Int): Float {
        if (vw <= 0 || outW * scale <= vw.toFloat()) return outW * 0.5f
        val halfVis = (vw * 0.5f) / scale
        return cx.coerceIn(halfVis, outW - halfVis)
    }

    fun clampCenterY(cy: Float, scale: Float, vh: Int): Float {
        if (vh <= 0 || outH * scale <= vh.toFloat()) return outH * 0.5f
        val halfVis = (vh * 0.5f) / scale
        return cy.coerceIn(halfVis, outH - halfVis)
    }

    // Initialize camera to Fit once viewport & image dimensions are known
    LaunchedEffect(tileController?.isReady, viewportSize) {
        if (tileController?.isReady == true && viewportSize.width > 0 && viewportSize.height > 0) {
            if (scalePxPerImgPx <= 0f) {
                scalePxPerImgPx = fitScale
                centerImgX = outW * 0.5f
                centerImgY = outH * 0.5f
            } else {
                val clampedS = scalePxPerImgPx.coerceIn(fitScale, maxScale)
                scalePxPerImgPx = clampedS
                centerImgX = clampCenterX(centerImgX, clampedS, viewportSize.width)
                centerImgY = clampCenterY(centerImgY, clampedS, viewportSize.height)
            }
        }
    }

    // Stream visible full-res tiles whenever camera or viewport moves
    val effectiveScale = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
    LaunchedEffect(
        tileController?.isReady,
        viewportSize,
        effectiveScale,
        centerImgX,
        centerImgY
    ) {
        val ctrl = tileController ?: return@LaunchedEffect
        if (!ctrl.isReady || viewportSize.width <= 0 || viewportSize.height <= 0) return@LaunchedEffect
        val halfW = (viewportSize.width * 0.5f) / effectiveScale
        val halfH = (viewportSize.height * 0.5f) / effectiveScale
        val visRect = Rect(
            left = centerImgX - halfW,
            top = centerImgY - halfH,
            right = centerImgX + halfW,
            bottom = centerImgY + halfH
        )
        ctrl.updateVisibleViewport(visRect, effectiveScale)
    }

    fun animateCameraTo(targetScale: Float, targetCx: Float, targetCy: Float) {
        cameraAnimJob?.cancel()
        val startScale = effectiveScale
        val startCx = centerImgX
        val startCy = centerImgY
        val clampedTargetS = targetScale.coerceIn(fitScale, maxScale)
        val clampedTargetCx = clampCenterX(targetCx, clampedTargetS, viewportSize.width)
        val clampedTargetCy = clampCenterY(targetCy, clampedTargetS, viewportSize.height)

        cameraAnimJob = coroutineScope.launch {
            animate(
                initialValue = 0f,
                targetValue = 1f,
                animationSpec = tween(durationMillis = 220)
            ) { frac, _ ->
                val s = startScale + (clampedTargetS - startScale) * frac
                scalePxPerImgPx = s
                centerImgX = clampCenterX(startCx + (clampedTargetCx - startCx) * frac, s, viewportSize.width)
                centerImgY = clampCenterY(startCy + (clampedTargetCy - startCy) * frac, s, viewportSize.height)
            }
        }
    }

    val errNoShare = stringResource(R.string.error_no_share_app)
    val errNoGallery = stringResource(R.string.error_no_gallery_app)
    val shareTitle = stringResource(R.string.share_chooser_title)

    val zoomPercent = (effectiveScale * 100f).roundToInt().coerceAtLeast(1)
    val isAtOneToOne = abs(effectiveScale - 1.0f) < 0.03f
    val isZoomedFromFit = effectiveScale > fitScale * 1.03f

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
                .padding(horizontal = 16.dp, vertical = 6.dp)
        ) {
            // True Full-Res Tiled Deep-Zoom Viewport
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0xFF06090F))
                    .onSizeChanged { viewportSize = it },
                contentAlignment = Alignment.Center
            ) {
                val ctrl = tileController
                if (ctrl == null || (!ctrl.isReady && !ctrl.loadFailed)) {
                    CircularProgressIndicator()
                } else if (ctrl.loadFailed) {
                    Text(
                        text = stringResource(R.string.err_decode_source),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error
                    )
                } else {
                    val baseAfter = ctrl.baseAfterImage
                    val baseOrig = ctrl.baseOriginalImage
                    val afterTiles = ctrl.activeAfterTiles
                    val origTiles = ctrl.activeOriginalTiles
                    // Read tileGeneration so crossfade completion triggers crisp final frame
                    val unusedGen = ctrl.tileGeneration
                    val filterQuality = if (useNearestFilter) FilterQuality.None else FilterQuality.Low

                    if (baseAfter != null && baseOrig != null && unusedGen >= 0L) {
                        Canvas(
                            modifier = Modifier
                                .fillMaxSize()
                                .pointerInput(item.id, fitScale, maxScale, outW, outH) {
                                    val handleHitRadiusPx = 34.dp.toPx()
                                    val tapSlopPx = viewConfiguration.touchSlop
                                    val doubleTapDistPx = 42.dp.toPx()

                                    var lastTapUpTimeMs = 0L
                                    var lastTapPos = Offset.Zero

                                    awaitEachGesture {
                                        val down = awaitFirstDown(requireUnconsumed = false)
                                        cameraAnimJob?.cancel()

                                        val downTimeMs = SystemClock.uptimeMillis()
                                        val curDownS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                                        val splitX = size.width * sliderFraction
                                        val handleCenter = Offset(splitX, size.height * 0.5f)
                                        val nearHandle = abs(down.position.x - splitX) <= handleHitRadiusPx &&
                                            (curDownS <= fitScale * 1.03f ||
                                                hypot(
                                                    down.position.x - handleCenter.x,
                                                    down.position.y - handleCenter.y
                                                ) <= handleHitRadiusPx * 1.6f ||
                                                abs(down.position.x - splitX) <= 20.dp.toPx())

                                        var draggingDivider = nearHandle
                                        var movedBeyondTap = false
                                        var hadMultiTouch = false
                                        val velocityTracker = VelocityTracker()
                                        velocityTracker.addPosition(down.uptimeMillis, down.position)

                                        do {
                                            val event = awaitPointerEvent()
                                            val pressed = event.changes.filter { it.pressed }

                                            if (pressed.size >= 2) {
                                                hadMultiTouch = true
                                                draggingDivider = false
                                                movedBeyondTap = true

                                                val zoomChange = event.calculateZoom()
                                                val panChange = event.calculatePan()
                                                val centroid = event.calculateCentroid(useCurrent = false)
                                                val centroidSize = event.calculateCentroidSize(useCurrent = false)

                                                if (centroidSize > 0f || zoomChange != 1f || panChange != Offset.Zero) {
                                                    val curS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                                                    val newS = (curS * zoomChange).coerceIn(fitScale, maxScale)

                                                    // Anchor image point under pinch centroid
                                                    val focalImgX = centerImgX + (centroid.x - size.width * 0.5f) / curS
                                                    val focalImgY = centerImgY + (centroid.y - size.height * 0.5f) / curS

                                                    val rawNewCx = focalImgX - (centroid.x + panChange.x - size.width * 0.5f) / newS
                                                    val rawNewCy = focalImgY - (centroid.y + panChange.y - size.height * 0.5f) / newS

                                                    scalePxPerImgPx = newS
                                                    centerImgX = clampCenterX(rawNewCx, newS, size.width)
                                                    centerImgY = clampCenterY(rawNewCy, newS, size.height)
                                                }
                                                event.changes.forEach {
                                                    if (it.positionChanged()) it.consume()
                                                }
                                            } else if (pressed.size == 1 && !hadMultiTouch) {
                                                val chg = pressed[0]
                                                val distFromDown = hypot(
                                                    chg.position.x - down.position.x,
                                                    chg.position.y - down.position.y
                                                )
                                                if (distFromDown > tapSlopPx) {
                                                    movedBeyondTap = true
                                                }

                                                if (draggingDivider && size.width > 0) {
                                                    sliderFraction = (chg.position.x / size.width.toFloat())
                                                        .coerceIn(0.02f, 0.98f)
                                                    if (chg.positionChanged()) chg.consume()
                                                } else if (movedBeyondTap) {
                                                    val curS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                                                    if (curS > fitScale * 1.01f) {
                                                        val delta = chg.position - chg.previousPosition
                                                        velocityTracker.addPosition(chg.uptimeMillis, chg.position)
                                                        centerImgX = clampCenterX(
                                                            centerImgX - delta.x / curS,
                                                            curS,
                                                            size.width
                                                        )
                                                        centerImgY = clampCenterY(
                                                            centerImgY - delta.y / curS,
                                                            curS,
                                                            size.height
                                                        )
                                                        if (chg.positionChanged()) chg.consume()
                                                    } else if (size.width > 0) {
                                                        // At Fit zoom, horizontal drag anywhere smoothly moves the split!
                                                        sliderFraction = (chg.position.x / size.width.toFloat())
                                                            .coerceIn(0.02f, 0.98f)
                                                        if (chg.positionChanged()) chg.consume()
                                                    }
                                                }
                                            }
                                        } while (event.changes.any { it.pressed })

                                        val upTimeMs = SystemClock.uptimeMillis()
                                        if (!movedBeyondTap && !hadMultiTouch && !draggingDivider &&
                                            upTimeMs - downTimeMs < 260L
                                        ) {
                                            // Check double-tap zoom!
                                            val dt = upTimeMs - lastTapUpTimeMs
                                            val dpos = hypot(
                                                down.position.x - lastTapPos.x,
                                                down.position.y - lastTapPos.y
                                            )
                                            if (dt in 35L..320L && dpos <= doubleTapDistPx) {
                                                lastTapUpTimeMs = 0L
                                                val curS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                                                val tappedImgX = centerImgX + (down.position.x - size.width * 0.5f) / curS
                                                val tappedImgY = centerImgY + (down.position.y - size.height * 0.5f) / curS

                                                val nextScale = when {
                                                    curS < 0.92f -> 1.0f // Jump straight to 1:1!
                                                    curS < 2.2f -> min(maxScale, 4.0f) // Jump to 400% (4x past 1:1)!
                                                    else -> fitScale // Reset to Fit!
                                                }
                                                if (nextScale <= fitScale * 1.02f) {
                                                    animateCameraTo(fitScale, outW * 0.5f, outH * 0.5f)
                                                } else {
                                                    animateCameraTo(nextScale, tappedImgX, tappedImgY)
                                                }
                                            } else {
                                                lastTapUpTimeMs = upTimeMs
                                                lastTapPos = down.position
                                            }
                                        } else if (movedBeyondTap && !hadMultiTouch && !draggingDivider) {
                                            // Smooth inertial pan fling
                                            val curS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                                            if (curS > fitScale * 1.02f) {
                                                val vel = velocityTracker.calculateVelocity()
                                                val speed = hypot(vel.x, vel.y)
                                                if (speed > 140f) {
                                                    val decay = exponentialDecay<Float>(frictionMultiplier = 1.7f)
                                                    val vw = size.width
                                                    val vh = size.height
                                                    cameraAnimJob = coroutineScope.launch {
                                                        launch {
                                                            var prevVal = 0f
                                                            AnimationState(0f, vel.x).animateDecay(decay) {
                                                                val step = value - prevVal
                                                                prevVal = value
                                                                centerImgX = clampCenterX(
                                                                    centerImgX - step / scalePxPerImgPx.coerceAtLeast(0.01f),
                                                                    scalePxPerImgPx.coerceAtLeast(0.01f),
                                                                    vw
                                                                )
                                                            }
                                                        }
                                                        launch {
                                                            var prevVal = 0f
                                                            AnimationState(0f, vel.y).animateDecay(decay) {
                                                                val step = value - prevVal
                                                                prevVal = value
                                                                centerImgY = clampCenterY(
                                                                    centerImgY - step / scalePxPerImgPx.coerceAtLeast(0.01f),
                                                                    scalePxPerImgPx.coerceAtLeast(0.01f),
                                                                    vh
                                                                )
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                        ) {
                            val cw = size.width
                            val ch = size.height
                            if (cw <= 0f || ch <= 0f) return@Canvas

                            val curS = if (scalePxPerImgPx > 0f) scalePxPerImgPx else fitScale
                            val nowMs = SystemClock.uptimeMillis()

                            // Full image bounding box on screen
                            val fullLeft = cw * 0.5f - centerImgX * curS
                            val fullTop = ch * 0.5f - centerImgY * curS
                            val fullRight = fullLeft + outW * curS
                            val fullBottom = fullTop + outH * curS

                            val baseDstOffset = IntOffset(fullLeft.roundToInt(), fullTop.roundToInt())
                            val baseDstSize = IntSize(
                                (fullRight - fullLeft).roundToInt().coerceAtLeast(1),
                                (fullBottom - fullTop).roundToInt().coerceAtLeast(1)
                            )

                            // 1. AFTER layer (full viewport): Base + full-res decoded tiles with crossfade
                            drawImage(
                                image = baseAfter,
                                srcOffset = IntOffset.Zero,
                                srcSize = IntSize(baseAfter.width, baseAfter.height),
                                dstOffset = baseDstOffset,
                                dstSize = baseDstSize,
                                filterQuality = FilterQuality.Low
                            )
                            drawTileLayer(
                                tiles = afterTiles,
                                fullLeft = fullLeft,
                                fullTop = fullTop,
                                scale = curS,
                                nowUptimeMs = nowMs,
                                filterQuality = filterQuality
                            )

                            // 2. BEFORE ("Original", bicubic-upscaled to exact same scale) clipped to left of divider
                            val splitX = cw * sliderFraction
                            clipRect(left = 0f, top = 0f, right = splitX, bottom = ch) {
                                drawImage(
                                    image = baseOrig,
                                    srcOffset = IntOffset.Zero,
                                    srcSize = IntSize(baseOrig.width, baseOrig.height),
                                    dstOffset = baseDstOffset,
                                    dstSize = baseDstSize,
                                    filterQuality = filterQuality
                                )
                                drawTileLayer(
                                    tiles = origTiles,
                                    fullLeft = fullLeft,
                                    fullTop = fullTop,
                                    scale = curS,
                                    nowUptimeMs = nowMs,
                                    filterQuality = filterQuality
                                )
                            }

                            // 3. Crisp split line + center drag handle
                            drawLine(
                                color = Color.White,
                                start = Offset(splitX, 0f),
                                end = Offset(splitX, ch),
                                strokeWidth = 2.dp.toPx()
                            )

                            val centerHandle = Offset(splitX, ch * 0.5f)
                            drawCircle(
                                color = Color(0xCC0F172A),
                                radius = 18.dp.toPx(),
                                center = centerHandle
                            )
                            drawCircle(
                                color = Color.White,
                                radius = 16.dp.toPx(),
                                center = centerHandle,
                                style = Stroke(width = 2.dp.toPx())
                            )

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

                        // Physical Left = "Original", Physical Right = "Enhanced" badges
                        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .align(Alignment.TopCenter)
                                    .padding(10.dp),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                OverlayTag(
                                    text = "${stringResource(R.string.label_original)} (${item.sourceWidth}×${item.sourceHeight})"
                                )
                                OverlayTag(
                                    text = "${stringResource(R.string.label_after)} (${outW}×${outH})"
                                )
                            }

                            // Bottom Zoom HUD: Zoom %, interactive "1:1" indicator, Fit, and Nearest/Bilinear toggle
                            Surface(
                                shape = RoundedCornerShape(20.dp),
                                color = Color(0xD90B1220),
                                border = BorderStroke(1.dp, Color(0x33FFFFFF)),
                                modifier = Modifier
                                    .align(Alignment.BottomCenter)
                                    .padding(bottom = 10.dp, start = 10.dp, end = 10.dp)
                            ) {
                                Row(
                                    modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                                ) {
                                    Text(
                                        text = stringResource(R.string.hud_zoom_pct, zoomPercent),
                                        style = MaterialTheme.typography.labelMedium,
                                        fontWeight = FontWeight.Bold,
                                        color = Color.White
                                    )

                                    HudPillButton(
                                        text = stringResource(R.string.zoom_one_to_one),
                                        active = isAtOneToOne,
                                        onClick = {
                                            animateCameraTo(1.0f, centerImgX, centerImgY)
                                        }
                                    )

                                    if (isZoomedFromFit) {
                                        HudPillButton(
                                            text = stringResource(R.string.reset_zoom),
                                            active = false,
                                            onClick = {
                                                animateCameraTo(fitScale, outW * 0.5f, outH * 0.5f)
                                            }
                                        )
                                    }

                                    HudPillButton(
                                        text = if (useNearestFilter) {
                                            stringResource(R.string.filter_nearest)
                                        } else {
                                            stringResource(R.string.filter_bilinear)
                                        },
                                        active = useNearestFilter,
                                        onClick = { useNearestFilter = !useNearestFilter }
                                    )
                                }
                            }
                        }
                    }
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            if (item.usedCpuFallback) {
                WarningInlineBox(text = stringResource(R.string.fallback_notice))
                Spacer(modifier = Modifier.height(6.dp))
            }

            Text(
                text = stringResource(
                    R.string.deep_zoom_fullres_note,
                    outW,
                    outH,
                    item.outputFileName
                ),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Per-image Re-process with different settings button
            FilledTonalButton(
                onClick = { showReprocessSheet = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(
                    imageVector = Icons.Default.Refresh,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = stringResource(R.string.btn_reprocess),
                    fontWeight = FontWeight.SemiBold
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Share & Open in Gallery buttons
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
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

            Spacer(modifier = Modifier.height(4.dp))
        }
    }

    if (showReprocessSheet) {
        ReprocessSettingsSheet(
            item = item,
            initialSettings = item.appliedSettings ?: defaultSettings,
            deviceMemory = deviceMemory,
            onDismiss = { showReprocessSheet = false },
            onConfirmReprocess = { chosenSettings ->
                showReprocessSheet = false
                onReprocessWithSettings(chosenSettings)
            }
        )
    }
}

private fun DrawScope.drawTileLayer(
    tiles: List<RenderTile>,
    fullLeft: Float,
    fullTop: Float,
    scale: Float,
    nowUptimeMs: Long,
    filterQuality: FilterQuality
) {
    for (tile in tiles) {
        if (tile.bitmap.isRecycled) continue
        val left = (fullLeft + tile.imageRect.left * scale).roundToInt()
        val top = (fullTop + tile.imageRect.top * scale).roundToInt()
        val right = (fullLeft + tile.imageRect.right * scale).roundToInt()
        val bottom = (fullTop + tile.imageRect.bottom * scale).roundToInt()
        val w = (right - left).coerceAtLeast(1)
        val h = (bottom - top).coerceAtLeast(1)

        val elapsed = (nowUptimeMs - tile.loadedAtUptimeMs).coerceAtLeast(0L)
        val alpha = if (elapsed >= TiledDeepZoomController.CROSSFADE_DURATION_MS) {
            1f
        } else {
            (elapsed.toFloat() / TiledDeepZoomController.CROSSFADE_DURATION_MS.toFloat()).coerceIn(0.15f, 1f)
        }

        drawImage(
            image = tile.imageBitmap,
            srcOffset = IntOffset.Zero,
            srcSize = IntSize(tile.imageBitmap.width, tile.imageBitmap.height),
            dstOffset = IntOffset(left, top),
            dstSize = IntSize(w, h),
            alpha = alpha,
            filterQuality = filterQuality
        )
    }
}

@Composable
private fun HudPillButton(
    text: String,
    active: Boolean,
    onClick: () -> Unit
) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = if (active) MaterialTheme.colorScheme.primary else Color(0x26FFFFFF),
        modifier = Modifier
            .clip(RoundedCornerShape(12.dp))
            .clickable(onClick = onClick)
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = if (active) MaterialTheme.colorScheme.onPrimary else Color.White,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp)
        )
    }
}

@Composable
private fun OverlayTag(text: String) {
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = Color(0xB80F172A)
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

@Composable
private fun ReprocessSettingsSheet(
    item: QueueItem,
    initialSettings: EnhancerSettings,
    deviceMemory: DeviceMemorySnapshot,
    onDismiss: () -> Unit,
    onConfirmReprocess: (EnhancerSettings) -> Unit
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var draft by remember { mutableStateOf(initialSettings) }

    val ramEstimate = remember(item.sourceWidth, item.sourceHeight, draft, deviceMemory) {
        MemoryEstimator.estimateForImage(
            srcWidth = item.sourceWidth,
            srcHeight = item.sourceHeight,
            settings = draft,
            deviceMem = deviceMemory
        )
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surface
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .navigationBarsPadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 8.dp)
        ) {
            Text(
                text = stringResource(R.string.reprocess_sheet_title),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
            Spacer(modifier = Modifier.height(2.dp))
            Text(
                text = stringResource(
                    R.string.reprocess_sheet_subtitle,
                    item.sourceWidth,
                    item.sourceHeight
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(modifier = Modifier.height(10.dp))

            // Live output resolution & RAM preview badge
            Surface(
                shape = RoundedCornerShape(10.dp),
                color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = stringResource(
                            R.string.res_arrow,
                            item.sourceWidth,
                            item.sourceHeight,
                            ramEstimate.plan.finalWidth,
                            ramEstimate.plan.finalHeight
                        ),
                        style = MaterialTheme.typography.labelMedium,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.primary
                    )
                    Text(
                        text = stringResource(R.string.est_ram_label, ramEstimate.estimatedPeakMb),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Model selector
            Text(
                text = stringResource(R.string.label_model),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(6.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                UpscaleModel.entries.forEach { model ->
                    FilterChip(
                        selected = draft.model == model,
                        onClick = { draft = draft.copy(model = model) },
                        label = { Text(stringResource(model.labelRes)) }
                    )
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // Target selector
            Text(
                text = stringResource(R.string.label_target),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(6.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TargetPreset.entries.forEach { preset ->
                    val label = when (preset) {
                        TargetPreset.NATIVE -> stringResource(R.string.target_native, draft.model.scale)
                        TargetPreset.UHD_4K -> stringResource(R.string.target_4k)
                        TargetPreset.UHD_8K -> stringResource(R.string.target_8k)
                    }
                    FilterChip(
                        selected = draft.target == preset,
                        onClick = { draft = draft.copy(target = preset) },
                        label = { Text(label) }
                    )
                }
            }

            Spacer(modifier = Modifier.height(10.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.2f))
            Spacer(modifier = Modifier.height(10.dp))

            // Max Quality toggle
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = stringResource(R.string.label_max_quality),
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.SemiBold
                    )
                    Text(
                        text = stringResource(R.string.max_quality_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Spacer(modifier = Modifier.width(12.dp))
                Switch(
                    checked = draft.maxQuality,
                    onCheckedChange = { draft = draft.copy(maxQuality = it) }
                )
            }

            Spacer(modifier = Modifier.height(10.dp))

            // TTA x8 toggle
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = stringResource(R.string.label_tta_x8),
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.SemiBold
                    )
                    Text(
                        text = stringResource(R.string.tta_x8_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Spacer(modifier = Modifier.width(12.dp))
                Switch(
                    checked = draft.ttaX8,
                    onCheckedChange = { draft = draft.copy(ttaX8 = it) }
                )
            }

            if (draft.ttaX8) {
                Spacer(modifier = Modifier.height(6.dp))
                WarningInlineBox(text = stringResource(R.string.warn_tta_slow))
            }

            Spacer(modifier = Modifier.height(10.dp))

            // Sharpness slider (0..100)
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = stringResource(R.string.label_sharpness),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary
                )
                Text(
                    text = if (draft.sharpness == 0) {
                        stringResource(R.string.sharpness_off)
                    } else {
                        stringResource(R.string.sharpness_value, draft.sharpness)
                    },
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold
                )
            }
            Slider(
                value = draft.sharpness.toFloat(),
                onValueChange = { draft = draft.copy(sharpness = it.roundToInt().coerceIn(0, 100)) },
                valueRange = 0f..100f
            )

            // Output format + JPEG 90..100 (4:4:4) slider
            Text(
                text = stringResource(R.string.label_format),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(6.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutputFormat.entries.forEach { fmt ->
                    val label = when (fmt) {
                        OutputFormat.JPEG -> stringResource(R.string.format_jpeg)
                        OutputFormat.PNG -> stringResource(R.string.format_png)
                    }
                    FilterChip(
                        selected = draft.format == fmt,
                        onClick = { draft = draft.copy(format = fmt) },
                        label = { Text(label) }
                    )
                }
            }

            if (draft.format == OutputFormat.JPEG) {
                Spacer(modifier = Modifier.height(6.dp))
                Text(
                    text = stringResource(R.string.label_jpeg_quality, draft.jpegQuality),
                    style = MaterialTheme.typography.labelMedium
                )
                Slider(
                    value = draft.jpegQuality.toFloat(),
                    onValueChange = { draft = draft.copy(jpegQuality = it.roundToInt().coerceIn(90, 100)) },
                    valueRange = 90f..100f,
                    steps = 9
                )
            } else {
                Spacer(modifier = Modifier.height(6.dp))
                WarningInlineBox(text = stringResource(R.string.png_slow_huge_warning))
            }

            if (ramEstimate.isTooHighForDevice) {
                Spacer(modifier = Modifier.height(6.dp))
                WarningInlineBox(
                    text = stringResource(
                        R.string.warn_high_ram,
                        ramEstimate.estimatedPeakMb,
                        deviceMemory.availMb
                    )
                )
            }

            Spacer(modifier = Modifier.height(14.dp))

            Button(
                onClick = { onConfirmReprocess(draft) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(
                    imageVector = Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(18.dp)
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = stringResource(R.string.reprocess_start_btn),
                    fontWeight = FontWeight.Bold
                )
            }

            Spacer(modifier = Modifier.height(16.dp))
        }
    }
}
