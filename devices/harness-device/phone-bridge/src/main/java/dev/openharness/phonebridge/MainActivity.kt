package dev.openharness.phonebridge

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** Minimal status screen: permissions, Start, Stop, and the state the service reports. */
class MainActivity : Activity() {

    private lateinit var status: TextView
    private val listener: (BridgeSnapshot) -> Unit = { s -> runOnUiThread { render(s) } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        status = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 14f
        }
        val start = Button(this).apply {
            text = "Start bridge"
            setOnClickListener { requestThenStart() }
        }
        val stop = Button(this).apply {
            text = "Stop"
            setOnClickListener {
                startService(Intent(this@MainActivity, BridgeService::class.java).setAction(BridgeService.ACTION_STOP))
            }
        }
        column.addView(status, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        column.addView(start)
        column.addView(stop)
        setContentView(column)
    }

    override fun onResume() {
        super.onResume()
        BridgeState.addListener(listener)
        render(BridgeState.snapshot)
    }

    override fun onPause() {
        BridgeState.removeListener(listener)
        super.onPause()
    }

    private fun render(s: BridgeSnapshot) {
        status.text = buildString {
            appendLine("state: ${s.phase.name.lowercase()}")
            if (s.detail.isNotEmpty()) appendLine(s.detail)
            appendLine("dial -> tunnel: ${s.bytesToTunnel} bytes")
            appendLine("tunnel -> dial: ${s.bytesToDial} bytes")
            appendLine()
            appendLine("tunnel: ${BuildConfig.TUNNEL_URL}")
            append("unbonded channel allowed: ${BuildConfig.DEV_ALLOW_UNBONDED}")
        }
    }

    private fun requestThenStart() {
        val wanted = arrayOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.POST_NOTIFICATIONS,
        )
        val missing = wanted.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isEmpty()) startBridge() else requestPermissions(missing.toTypedArray(), REQUEST_PERMISSIONS)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_PERMISSIONS) return
        // Notifications are optional. Bluetooth is checked again by the service, which shows a state if it is denied.
        startBridge()
    }

    private fun startBridge() {
        startForegroundService(Intent(this, BridgeService::class.java).setAction(BridgeService.ACTION_START))
    }

    companion object {
        private const val REQUEST_PERMISSIONS = 7
    }
}
