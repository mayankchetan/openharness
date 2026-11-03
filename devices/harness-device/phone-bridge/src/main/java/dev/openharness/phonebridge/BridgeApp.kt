package dev.openharness.phonebridge

import android.app.Application
import android.net.ConnectivityManager
import android.net.NetworkCapabilities

/** Installs the publisher before any activity or service runs, so no BridgeService change is needed. */
class BridgeApp : Application() {
    override fun onCreate() {
        super.onCreate()
        val cm = getSystemService(ConnectivityManager::class.java)
        val publisher = StatusPublisher {
            val caps = cm.getNetworkCapabilities(cm.activeNetwork)
            caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
                caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
        }
        StatusPublisher.shared = publisher
        BridgeState.addListener { publisher.onState(it) }
    }
}
