package com.enhancer.app.util

import android.content.Context
import android.os.Build
import android.os.PowerManager
import androidx.annotation.RequiresApi
import androidx.annotation.StringRes
import com.enhancer.app.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class ThermalState(
    val isSevere: Boolean = false,
    @StringRes val statusLabelRes: Int = R.string.thermal_status_severe,
    val rawStatus: Int = 0
)

class ThermalSafetyMonitor(context: Context) {

    private val powerManager = context.applicationContext.getSystemService(Context.POWER_SERVICE) as? PowerManager
    private val _thermalState = MutableStateFlow(readCurrentState())
    val thermalState: StateFlow<ThermalState> = _thermalState.asStateFlow()

    private var listenerRegistered = false
    private var platformListener: Any? = null

    fun refreshNow(): ThermalState {
        val current = readCurrentState()
        _thermalState.value = current
        return current
    }

    fun startListening() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || listenerRegistered) {
            return
        }
        val pm = powerManager ?: return
        registerApi29Listener(pm)
    }

    fun stopListening() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || !listenerRegistered) {
            return
        }
        val pm = powerManager ?: return
        unregisterApi29Listener(pm)
    }

    private fun readCurrentState(): ThermalState {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return ThermalState(isSevere = false)
        }
        val pm = powerManager ?: return ThermalState(isSevere = false)
        return mapThermalStatusApi29(runCatching { pm.currentThermalStatus }.getOrDefault(0))
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun registerApi29Listener(pm: PowerManager) {
        val listener = PowerManager.OnThermalStatusChangedListener { status ->
            _thermalState.value = mapThermalStatusApi29(status)
        }
        runCatching {
            pm.addThermalStatusListener(listener)
            platformListener = listener
            listenerRegistered = true
        }
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun unregisterApi29Listener(pm: PowerManager) {
        val listener = platformListener as? PowerManager.OnThermalStatusChangedListener ?: return
        runCatching {
            pm.removeThermalStatusListener(listener)
        }
        platformListener = null
        listenerRegistered = false
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun mapThermalStatusApi29(status: Int): ThermalState {
        val isSevere = status >= PowerManager.THERMAL_STATUS_SEVERE
        val labelRes = when {
            status >= PowerManager.THERMAL_STATUS_EMERGENCY -> R.string.thermal_status_emergency
            status >= PowerManager.THERMAL_STATUS_CRITICAL -> R.string.thermal_status_critical
            else -> R.string.thermal_status_severe
        }
        return ThermalState(
            isSevere = isSevere,
            statusLabelRes = labelRes,
            rawStatus = status
        )
    }
}
