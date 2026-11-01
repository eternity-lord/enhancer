package com.enhancer.app

import android.app.Application
import com.enhancer.app.data.EnhancerQueueRepository
import com.enhancer.app.util.ThermalSafetyMonitor

class EnhancerApplication : Application() {

    lateinit var queueRepository: EnhancerQueueRepository
        private set

    lateinit var thermalMonitor: ThermalSafetyMonitor
        private set

    override fun onCreate() {
        super.onCreate()
        queueRepository = EnhancerQueueRepository(this)
        thermalMonitor = ThermalSafetyMonitor(this)
    }
}
