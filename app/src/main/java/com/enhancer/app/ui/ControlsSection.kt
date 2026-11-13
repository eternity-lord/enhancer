@file:OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)

package com.enhancer.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AutoFixHigh
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.FaceRetouchingNatural
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.WarningAmber
import androidx.compose.material.icons.filled.WbSunny
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.enhancer.app.R
import com.enhancer.app.model.EnhanceModePreset
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
import com.enhancer.app.model.TargetPreset
import com.enhancer.app.model.TileSizeOption
import com.enhancer.app.model.UpscaleModel
import kotlin.math.roundToInt

@Composable
fun ControlsSection(
    settings: EnhancerSettings,
    onSettingsChanged: (EnhancerSettings) -> Unit,
    enabled: Boolean = true,
    initiallyExpanded: Boolean = true,
    modifier: Modifier = Modifier
) {
    var expanded by rememberSaveable { mutableStateOf(initiallyExpanded) }
    var advancedExpanded by rememberSaveable { mutableStateOf(false) }

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { expanded = !expanded },
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(
                    imageVector = Icons.Default.Tune,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary
                )
                Spacer(modifier = Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = stringResource(R.string.settings_title),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold
                    )
                    Text(
                        text = "${stringResource(settings.enhanceMode.labelRes)} • ${stringResource(settings.model.labelRes)}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.primary
                    )
                }
                Icon(
                    imageVector = if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            if (expanded) {
                Spacer(modifier = Modifier.height(14.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
                Spacer(modifier = Modifier.height(14.dp))

                // 1. Let's Enhance 7 AI Studio Modes
                SectionLabel(text = stringResource(R.string.label_enhance_mode))
                Spacer(modifier = Modifier.height(8.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    EnhanceModePreset.entries.forEach { preset ->
                        val isSelected = settings.enhanceMode == preset
                        FilterChip(
                            selected = isSelected,
                            onClick = { onSettingsChanged(settings.withModePreset(preset)) },
                            enabled = enabled,
                            leadingIcon = if (isSelected) {
                                {
                                    Icon(
                                        imageVector = Icons.Default.AutoFixHigh,
                                        contentDescription = null,
                                        modifier = Modifier.size(16.dp)
                                    )
                                }
                            } else null,
                            label = {
                                Text(
                                    text = stringResource(preset.labelRes),
                                    fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal
                                )
                            }
                        )
                    }
                }

                Spacer(modifier = Modifier.height(6.dp))
                Surface(
                    shape = RoundedCornerShape(10.dp),
                    color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.35f),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text(
                        text = stringResource(settings.enhanceMode.descriptionRes),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)
                    )
                }

                // 1B. Ultra Mode Multi-Scale Sliders (Intensity & Size of Changes)
                if (settings.enhanceMode == EnhanceModePreset.ULTRA) {
                    Spacer(modifier = Modifier.height(12.dp))
                    Surface(
                        shape = RoundedCornerShape(12.dp),
                        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f),
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.35f)),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Column(modifier = Modifier.padding(12.dp)) {
                            Text(
                                text = stringResource(R.string.label_ultra_intensity, settings.ultraIntensity),
                                style = MaterialTheme.typography.labelLarge,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.primary
                            )
                            Text(
                                text = stringResource(R.string.ultra_intensity_desc),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                            Slider(
                                value = settings.ultraIntensity.toFloat(),
                                onValueChange = {
                                    onSettingsChanged(settings.copy(ultraIntensity = it.roundToInt().coerceIn(0, 100)))
                                },
                                valueRange = 0f..100f,
                                enabled = enabled
                            )

                            Spacer(modifier = Modifier.height(4.dp))

                            Text(
                                text = stringResource(R.string.label_ultra_size, settings.ultraSize),
                                style = MaterialTheme.typography.labelLarge,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.primary
                            )
                            Text(
                                text = stringResource(R.string.ultra_size_desc),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                            Slider(
                                value = settings.ultraSize.toFloat(),
                                onValueChange = {
                                    onSettingsChanged(settings.copy(ultraSize = it.roundToInt().coerceIn(0, 100)))
                                },
                                valueRange = 0f..100f,
                                enabled = enabled
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.height(14.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
                Spacer(modifier = Modifier.height(12.dp))

                // 2. Companion AI Engine A: Enhance Faces (Photo 2.0 — SCRFD + CodeFormer 512x512)
                Surface(
                    shape = RoundedCornerShape(12.dp),
                    color = if (settings.enhanceFaces) {
                        MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.28f)
                    } else {
                        MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.25f)
                    },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp)) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                imageVector = Icons.Default.FaceRetouchingNatural,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.primary,
                                modifier = Modifier.size(20.dp)
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    text = stringResource(R.string.label_enhance_faces),
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.SemiBold
                                )
                                Text(
                                    text = stringResource(R.string.enhance_faces_desc),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            Spacer(modifier = Modifier.width(10.dp))
                            Switch(
                                checked = settings.enhanceFaces,
                                onCheckedChange = { onSettingsChanged(settings.copy(enhanceFaces = it)) },
                                enabled = enabled
                            )
                        }

                        if (settings.enhanceFaces) {
                            Spacer(modifier = Modifier.height(6.dp))
                            Text(
                                text = stringResource(R.string.label_face_strength, settings.faceStrength),
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.Medium,
                                color = MaterialTheme.colorScheme.primary
                            )
                            Slider(
                                value = settings.faceStrength.toFloat(),
                                onValueChange = {
                                    onSettingsChanged(settings.copy(faceStrength = it.roundToInt().coerceIn(10, 100)))
                                },
                                valueRange = 10f..100f,
                                enabled = enabled
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.height(10.dp))

                // 3. Companion AI Engine B: Light AI (Smart HDR Relighting & Tone)
                Surface(
                    shape = RoundedCornerShape(12.dp),
                    color = if (settings.lightAi) {
                        MaterialTheme.colorScheme.tertiaryContainer.copy(alpha = 0.28f)
                    } else {
                        MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.25f)
                    },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp)) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                imageVector = Icons.Default.WbSunny,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.primary,
                                modifier = Modifier.size(20.dp)
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    text = stringResource(R.string.label_light_ai),
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.SemiBold
                                )
                                Text(
                                    text = stringResource(R.string.light_ai_desc),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            Spacer(modifier = Modifier.width(10.dp))
                            Switch(
                                checked = settings.lightAi,
                                onCheckedChange = { onSettingsChanged(settings.copy(lightAi = it)) },
                                enabled = enabled
                            )
                        }

                        if (settings.lightAi) {
                            Spacer(modifier = Modifier.height(6.dp))
                            Text(
                                text = stringResource(R.string.label_light_ai_strength, settings.lightAiStrength),
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.Medium,
                                color = MaterialTheme.colorScheme.primary
                            )
                            Slider(
                                value = settings.lightAiStrength.toFloat(),
                                onValueChange = {
                                    onSettingsChanged(settings.copy(lightAiStrength = it.roundToInt().coerceIn(10, 100)))
                                },
                                valueRange = 10f..100f,
                                enabled = enabled
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.height(14.dp))

                // 4. Target resolution (Native xN / 2K / 4K / 8K — Print-Ready 300 DPI)
                SectionLabel(text = stringResource(R.string.label_target))
                Spacer(modifier = Modifier.height(6.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    FilterChip(
                        selected = settings.target == TargetPreset.NATIVE,
                        onClick = { onSettingsChanged(settings.copy(target = TargetPreset.NATIVE)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_native, settings.model.scale)) }
                    )
                    FilterChip(
                        selected = settings.target == TargetPreset.QHD_2K,
                        onClick = { onSettingsChanged(settings.copy(target = TargetPreset.QHD_2K)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_2k)) }
                    )
                    FilterChip(
                        selected = settings.target == TargetPreset.UHD_4K,
                        onClick = { onSettingsChanged(settings.copy(target = TargetPreset.UHD_4K)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_4k)) }
                    )
                    FilterChip(
                        selected = settings.target == TargetPreset.UHD_8K,
                        onClick = { onSettingsChanged(settings.copy(target = TargetPreset.UHD_8K)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_8k)) }
                    )
                }

                Spacer(modifier = Modifier.height(10.dp))

                // 5. Output format (JPEG 4:4:4 300 DPI / PNG Lossless 300 DPI)
                SectionLabel(text = stringResource(R.string.label_format))
                Spacer(modifier = Modifier.height(6.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    FilterChip(
                        selected = settings.format == OutputFormat.JPEG,
                        onClick = { onSettingsChanged(settings.copy(format = OutputFormat.JPEG)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.format_jpeg)) }
                    )
                    FilterChip(
                        selected = settings.format == OutputFormat.PNG,
                        onClick = { onSettingsChanged(settings.copy(format = OutputFormat.PNG)) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.format_png)) }
                    )
                }

                if (settings.format == OutputFormat.JPEG) {
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(
                        text = stringResource(R.string.label_jpeg_quality, settings.jpegQuality),
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Slider(
                        value = settings.jpegQuality.toFloat(),
                        onValueChange = {
                            onSettingsChanged(settings.copy(jpegQuality = it.roundToInt().coerceIn(90, 100)))
                        },
                        valueRange = 90f..100f,
                        steps = 9,
                        enabled = enabled
                    )
                } else {
                    Spacer(modifier = Modifier.height(8.dp))
                    WarningInlineBox(text = stringResource(R.string.png_slow_huge_warning))
                }

                Spacer(modifier = Modifier.height(10.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
                Spacer(modifier = Modifier.height(10.dp))

                // 6. Collapsible Advanced Engine & Output Controls (100% Unlocked — zero grayed-out options!)
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { advancedExpanded = !advancedExpanded }
                        .padding(vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = stringResource(R.string.label_advanced_section),
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.weight(1f)
                    )
                    Icon(
                        imageVector = if (advancedExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary
                    )
                }

                if (advancedExpanded) {
                    Spacer(modifier = Modifier.height(10.dp))

                    // 6A. Neural Super-Resolution Engine Chooser (all 8 bundled models)
                    SectionLabel(text = stringResource(R.string.label_model))
                    Spacer(modifier = Modifier.height(8.dp))
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        UpscaleModel.entries.forEach { model ->
                            val isSelected = settings.model == model
                            Surface(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable(enabled = enabled) {
                                        onSettingsChanged(settings.copy(model = model))
                                    },
                                shape = RoundedCornerShape(12.dp),
                                color = if (isSelected) {
                                    MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.55f)
                                } else {
                                    MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f)
                                },
                                border = BorderStroke(
                                    width = 1.dp,
                                    color = if (isSelected) {
                                        MaterialTheme.colorScheme.primary
                                    } else {
                                        Color.Transparent
                                    }
                                )
                            ) {
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .padding(horizontal = 10.dp, vertical = 8.dp),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    RadioButton(
                                        selected = isSelected,
                                        onClick = if (enabled) ({ onSettingsChanged(settings.copy(model = model)) }) else null,
                                        enabled = enabled
                                    )
                                    Spacer(modifier = Modifier.width(6.dp))
                                    Column(modifier = Modifier.weight(1f)) {
                                        Text(
                                            text = stringResource(model.labelRes),
                                            style = MaterialTheme.typography.bodyMedium,
                                            fontWeight = FontWeight.Medium,
                                            color = MaterialTheme.colorScheme.onSurface
                                        )
                                        Text(
                                            text = stringResource(model.descriptionRes),
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                            }
                        }
                    }

                    Spacer(modifier = Modifier.height(14.dp))

                    // 6B. Pre-SR Denoise & JPEG Deblock slider (0..100%)
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        SectionLabel(text = stringResource(R.string.label_denoise))
                        Text(
                            text = if (settings.denoiseStrength == 0) {
                                stringResource(R.string.denoise_off)
                            } else {
                                stringResource(R.string.denoise_value, settings.denoiseStrength)
                            },
                            style = MaterialTheme.typography.labelLarge,
                            fontWeight = FontWeight.SemiBold,
                            color = MaterialTheme.colorScheme.primary
                        )
                    }
                    Text(
                        text = stringResource(R.string.denoise_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Slider(
                        value = settings.denoiseStrength.toFloat(),
                        onValueChange = {
                            onSettingsChanged(settings.copy(denoiseStrength = it.roundToInt().coerceIn(0, 100)))
                        },
                        valueRange = 0f..100f,
                        enabled = enabled
                    )

                    Spacer(modifier = Modifier.height(10.dp))

                    // 6C. Adaptive Edge Sharpness (0..100)
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        SectionLabel(text = stringResource(R.string.label_sharpness))
                        Text(
                            text = if (settings.sharpness == 0) {
                                stringResource(R.string.sharpness_off)
                            } else {
                                stringResource(R.string.sharpness_value, settings.sharpness)
                            },
                            style = MaterialTheme.typography.labelLarge,
                            fontWeight = FontWeight.SemiBold,
                            color = if (settings.sharpness > 0) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            }
                        )
                    }
                    Text(
                        text = stringResource(R.string.sharpness_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Slider(
                        value = settings.sharpness.toFloat(),
                        onValueChange = {
                            onSettingsChanged(settings.copy(sharpness = it.roundToInt().coerceIn(0, 100)))
                        },
                        valueRange = 0f..100f,
                        enabled = enabled
                    )

                    Spacer(modifier = Modifier.height(10.dp))

                    // 6D. Max Quality Supersampling (Always unlocked!)
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
                            checked = settings.maxQuality,
                            onCheckedChange = {
                                onSettingsChanged(settings.copy(maxQuality = it, safeMode = false))
                            },
                            enabled = enabled
                        )
                    }

                    Spacer(modifier = Modifier.height(10.dp))

                    // 6E. TTA x8 Mode (Always unlocked!)
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
                            checked = settings.ttaX8,
                            onCheckedChange = {
                                onSettingsChanged(settings.copy(ttaX8 = it, safeMode = false))
                            },
                            enabled = enabled
                        )
                    }

                    if (settings.ttaX8) {
                        Spacer(modifier = Modifier.height(8.dp))
                        WarningInlineBox(text = stringResource(R.string.warn_tta_slow))
                    }

                    Spacer(modifier = Modifier.height(12.dp))
                    HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
                    Spacer(modifier = Modifier.height(12.dp))

                    // 6F. Compute Backend (Auto GPU vs CPU Precision + One-tap Low-RAM Preset without lockouts)
                    SectionLabel(text = stringResource(R.string.label_compute_backend))
                    Spacer(modifier = Modifier.height(6.dp))
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        FilterChip(
                            selected = !settings.usesCpuOnly,
                            onClick = { onSettingsChanged(settings.copy(forceCpu = false, safeMode = false)) },
                            enabled = enabled,
                            label = { Text(stringResource(R.string.backend_auto_gpu)) }
                        )
                        FilterChip(
                            selected = settings.usesCpuOnly,
                            onClick = { onSettingsChanged(settings.copy(forceCpu = true, safeMode = false)) },
                            enabled = enabled,
                            label = { Text(stringResource(R.string.backend_force_cpu)) }
                        )
                    }

                    Spacer(modifier = Modifier.height(6.dp))
                    OutlinedButton(
                        onClick = { onSettingsChanged(settings.withLowRamPreset()) },
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(
                            imageVector = Icons.Default.Memory,
                            contentDescription = null,
                            modifier = Modifier.size(16.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(stringResource(R.string.btn_apply_low_ram_preset))
                    }

                    Spacer(modifier = Modifier.height(12.dp))

                    // 6G. Tile Size (Always unlocked!)
                    SectionLabel(text = stringResource(R.string.label_tile_size))
                    Spacer(modifier = Modifier.height(6.dp))
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        TileSizeOption.entries.forEach { option ->
                            val label = if (option == TileSizeOption.AUTO) {
                                stringResource(R.string.tile_auto)
                            } else {
                                stringResource(R.string.tile_px, option.effectivePixels)
                            }
                            FilterChip(
                                selected = settings.tileSize == option,
                                onClick = {
                                    onSettingsChanged(settings.copy(tileSize = option, safeMode = false))
                                },
                                enabled = enabled,
                                label = { Text(label) }
                            )
                        }
                    }

                    Spacer(modifier = Modifier.height(12.dp))

                    // 6H. Force FP32 Storage toggle
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = stringResource(R.string.label_disable_fp16),
                                style = MaterialTheme.typography.bodyMedium,
                                fontWeight = FontWeight.Medium
                            )
                            Text(
                                text = stringResource(R.string.disable_fp16_desc),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        Spacer(modifier = Modifier.width(12.dp))
                        Switch(
                            checked = settings.disableFp16,
                            onCheckedChange = { onSettingsChanged(settings.copy(disableFp16 = it)) },
                            enabled = enabled
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelLarge,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.primary
    )
}

@Composable
fun WarningInlineBox(
    text: String,
    isError: Boolean = false,
    modifier: Modifier = Modifier
) {
    val bgColor = if (isError) {
        MaterialTheme.colorScheme.errorContainer.copy(alpha = 0.65f)
    } else {
        Color(0xFF78350F).copy(alpha = 0.45f)
    }
    val contentColor = if (isError) {
        MaterialTheme.colorScheme.onErrorContainer
    } else {
        Color(0xFFFDE68A)
    }

    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(10.dp),
        color = bgColor
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = Icons.Default.WarningAmber,
                contentDescription = null,
                tint = contentColor
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = text,
                style = MaterialTheme.typography.bodySmall,
                color = contentColor
            )
        }
    }
}
