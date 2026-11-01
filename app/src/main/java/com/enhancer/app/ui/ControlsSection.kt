@file:OptIn(ExperimentalLayoutApi::class)

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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.WarningAmber
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
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
import com.enhancer.app.model.EnhancerSettings
import com.enhancer.app.model.OutputFormat
import com.enhancer.app.model.TargetPreset
import com.enhancer.app.model.TileSizeOption
import com.enhancer.app.model.UpscaleModel

@Composable
fun ControlsSection(
    settings: EnhancerSettings,
    enabled: Boolean,
    onModelSelected: (UpscaleModel) -> Unit,
    onTargetSelected: (TargetPreset) -> Unit,
    onFormatSelected: (OutputFormat) -> Unit,
    onTileSizeSelected: (TileSizeOption) -> Unit,
    onDisableFp16Changed: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    var expanded by rememberSaveable { mutableStateOf(true) }

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
                Text(
                    text = stringResource(R.string.settings_title),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f)
                )
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

                // 1. Model chooser
                SectionLabel(text = stringResource(R.string.label_model))
                Spacer(modifier = Modifier.height(8.dp))
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    UpscaleModel.entries.forEach { model ->
                        val isSelected = settings.model == model
                        Surface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable(enabled = enabled) { onModelSelected(model) },
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
                                    onClick = if (enabled) ({ onModelSelected(model) }) else null,
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

                Spacer(modifier = Modifier.height(16.dp))

                // 2. Target resolution
                SectionLabel(text = stringResource(R.string.label_target))
                Spacer(modifier = Modifier.height(6.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    FilterChip(
                        selected = settings.target == TargetPreset.NATIVE,
                        onClick = { onTargetSelected(TargetPreset.NATIVE) },
                        enabled = enabled,
                        label = {
                            Text(stringResource(R.string.target_native, settings.model.scale))
                        }
                    )
                    FilterChip(
                        selected = settings.target == TargetPreset.UHD_4K,
                        onClick = { onTargetSelected(TargetPreset.UHD_4K) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_4k)) }
                    )
                    FilterChip(
                        selected = settings.target == TargetPreset.UHD_8K,
                        onClick = { onTargetSelected(TargetPreset.UHD_8K) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.target_8k)) }
                    )
                }

                Spacer(modifier = Modifier.height(14.dp))

                // 3. Output format (JPEG / PNG)
                SectionLabel(text = stringResource(R.string.label_format))
                Spacer(modifier = Modifier.height(6.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    FilterChip(
                        selected = settings.format == OutputFormat.JPEG,
                        onClick = { onFormatSelected(OutputFormat.JPEG) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.format_jpeg)) }
                    )
                    FilterChip(
                        selected = settings.format == OutputFormat.PNG,
                        onClick = { onFormatSelected(OutputFormat.PNG) },
                        enabled = enabled,
                        label = { Text(stringResource(R.string.format_png)) }
                    )
                }

                if (settings.format == OutputFormat.PNG) {
                    Spacer(modifier = Modifier.height(8.dp))
                    WarningInlineBox(
                        text = stringResource(R.string.png_slow_huge_warning)
                    )
                }

                Spacer(modifier = Modifier.height(14.dp))

                // 4. Tile size
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
                            onClick = { onTileSizeSelected(option) },
                            enabled = enabled,
                            label = { Text(label) }
                        )
                    }
                }

                Spacer(modifier = Modifier.height(14.dp))
                HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
                Spacer(modifier = Modifier.height(12.dp))

                // 5. Disable fp16 toggle
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
                        onCheckedChange = onDisableFp16Changed,
                        enabled = enabled
                    )
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
