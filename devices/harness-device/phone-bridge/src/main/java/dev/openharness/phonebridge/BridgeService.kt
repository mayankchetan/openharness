package dev.openharness.phonebridge

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log

/**
 * Foreground service (type connectedDevice) that owns one relay session: find the dial, open the CoC, open the
 * tunnel on the first byte, pump. When the session ends it shows why and stops. There is no retry loop in this
 * version (rule 6: do not loop); reconnect and presence come with B10b.
 */
class BridgeService : Service() {

    private val main = Handler(Looper.getMainLooper())
    private var dialLink: DialLink? = null
    private var pump: Pump? = null
    private var session = 0 // bumped on every start, so a late callback of an old session is ignored

    private val ticker = object : Runnable {
        override fun run() {
            val p = pump ?: return
            val cur = BridgeState.snapshot
            if (cur.phase == Phase.RELAYING || cur.phase == Phase.CHANNEL_OPEN) {
                val phase = if (p.bytesToTunnel() > 0) Phase.RELAYING else cur.phase
                BridgeState.set(phase, cur.detail, p.bytesToTunnel(), p.bytesToDial())
            }
            main.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                session++ // a late callback of the stopped session must not start anything
                teardown()
                BridgeState.set(Phase.IDLE, "stopped")
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            else -> begin()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        teardown()
        super.onDestroy()
    }

    private fun begin() {
        teardown()
        val mySession = ++session

        // Always satisfy startForegroundService() first. The manifest declares CHANGE_NETWORK_STATE, a normal
        // permission that on its own meets the connectedDevice prerequisite, so this call cannot throw for a missing
        // Bluetooth permission. Skipping it would risk ForegroundServiceDidNotStartInTimeException. The Bluetooth
        // check comes right after; if it fails the service stops itself. The activity does not start the service at
        // all without those permissions, so this is the second line of defense.
        try {
            startForeground(NOTIFICATION_ID, notification("Starting"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (e: Exception) {
            Log.w(TAG, "startForeground refused: ${e.javaClass.simpleName}")
            BridgeState.set(Phase.FAILED, "foreground start refused: ${e.javaClass.simpleName}")
            stopSelf()
            return
        }

        if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED ||
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            finish(mySession, Phase.NO_PERMISSION, "Bluetooth permission denied")
            return
        }
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (adapter == null || !adapter.isEnabled) {
            finish(mySession, Phase.BLUETOOTH_OFF, "Bluetooth is off")
            return
        }

        val link = DialLink(this, BuildConfig.DEV_ALLOW_UNBONDED, object : DialLink.Listener {
            override fun onScanning() = update(mySession, Phase.SCANNING, "scanning for the dial")
            override fun onConnecting(name: String) = update(mySession, Phase.CONNECTING, "connecting to $name")
            override fun onChannel(channel: DialChannel) {
                main.post { startPump(mySession, channel) }
            }
            override fun onFailed(detail: String) {
                main.post { finish(mySession, Phase.FAILED, detail) } // also ends a running session (link lost)
            }
        })
        dialLink = link
        link.start()
    }

    private fun startPump(mySession: Int, channel: DialChannel) {
        if (mySession != session) {
            channel.close()
            return
        }
        update(mySession, Phase.CHANNEL_OPEN, "channel open, waiting for the dial")
        val p = Pump(channel, TunnelLink(BuildConfig.TUNNEL_URL), object : PumpListener {
            override fun onTunnelOpening() = update(mySession, Phase.RELAYING, "tunnel opening")
            override fun onTunnelOpen() = update(mySession, Phase.RELAYING, "tunnel up")
            override fun onClosed(reason: CloseReason) {
                main.post { finish(mySession, Phase.CLOSED, "session ended: ${reason.name.lowercase()}") }
            }
        })
        pump = p
        p.start()
        main.postDelayed(ticker, 1000)
    }

    private fun update(mySession: Int, phase: Phase, detail: String) {
        if (mySession != session) return
        val p = pump
        BridgeState.set(phase, detail, p?.bytesToTunnel() ?: 0, p?.bytesToDial() ?: 0)
        main.post { updateNotification(detail) }
    }

    /** End the session and stop the service, keeping the last text on the status screen. */
    private fun finish(mySession: Int, phase: Phase, detail: String) {
        if (mySession != session) return
        session++ // callbacks that teardown() triggers (pump closed, link closed) belong to the old session
        val up = pump?.bytesToTunnel() ?: 0
        val down = pump?.bytesToDial() ?: 0
        Log.i(TAG, "finish: $phase")
        teardown()
        BridgeState.set(phase, detail, up, down)
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun teardown() {
        main.removeCallbacks(ticker)
        pump?.stop()
        pump = null
        dialLink?.stop()
        dialLink = null
    }

    private fun notification(text: String): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL_ID, "Dial bridge", NotificationManager.IMPORTANCE_LOW))
        val stop = PendingIntent.getService(
            this, 0, Intent(this, BridgeService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val open = PendingIntent.getActivity(
            this, 1, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Dial bridge")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(text))
    }

    companion object {
        private const val TAG = "BridgeService"
        private const val CHANNEL_ID = "bridge"
        private const val NOTIFICATION_ID = 1
        const val ACTION_START = "dev.openharness.phonebridge.START"
        const val ACTION_STOP = "dev.openharness.phonebridge.STOP"
    }
}
