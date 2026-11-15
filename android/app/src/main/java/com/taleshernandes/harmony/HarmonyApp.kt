package com.taleshernandes.harmony

import android.app.Application
import android.util.Log
import com.taleshernandes.harmony.diag.Crash
import com.taleshernandes.harmony.diag.Diag

class HarmonyApp : Application() {
    lateinit var core: Core
        private set

    override fun onCreate() {
        super.onCreate()
        Diag.echo = { Log.i("harmony", it) }
        Crash.install(this) { if (::core.isInitialized) core.diagHeader() else "harmony" }
        core = Core(this)
        core.start()
    }
}
