package com.enhancer.app.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.res.Configuration
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.LayoutDirection
import com.enhancer.app.model.AppLanguageMode
import java.util.Locale

@Composable
fun LocalizedAppContainer(
    languageMode: AppLanguageMode,
    content: @Composable () -> Unit
) {
    val baseContext = LocalContext.current
    val baseConfig = LocalConfiguration.current

    if (languageMode == AppLanguageMode.SYSTEM) {
        val sysDir = if (baseConfig.layoutDirection == View.LAYOUT_DIRECTION_RTL) {
            LayoutDirection.Rtl
        } else {
            LayoutDirection.Ltr
        }
        CompositionLocalProvider(LocalLayoutDirection provides sysDir) {
            content()
        }
        return
    }

    val (localizedContext, layoutDir) = remember(baseContext, baseConfig, languageMode) {
        val locale = Locale(languageMode.tag)
        val newConfig = Configuration(baseConfig).apply {
            setLocale(locale)
            setLayoutDirection(locale)
        }
        val ctx = baseContext.createConfigurationContext(newConfig)
        val dir = if (languageMode == AppLanguageMode.FA) LayoutDirection.Rtl else LayoutDirection.Ltr
        ctx to dir
    }

    CompositionLocalProvider(
        LocalContext provides localizedContext,
        LocalConfiguration provides localizedContext.resources.configuration,
        LocalLayoutDirection provides layoutDir
    ) {
        content()
    }
}

fun Context.findActivity(): Activity? {
    var current: Context? = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

fun Context.startExternalActivitySafely(intent: Intent): Boolean {
    val activity = findActivity()
    return runCatching {
        if (activity != null) {
            activity.startActivity(intent)
        } else {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            startActivity(intent)
        }
        true
    }.getOrDefault(false)
}
