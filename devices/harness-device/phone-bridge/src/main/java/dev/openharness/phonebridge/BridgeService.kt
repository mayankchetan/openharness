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
 * tunnel on the first byte, pump. When a session ends for a reason that can get better (dial out of range, tunnel
 * down) it waits on the 1-2-4-8-16-30 s ladder and starts again; one ladder for the BLE link, one for the tunnel,
 * each reset by its own good connection. It stops for good on a user Stop, a missing permission, Bluetooth off or a
 * rejected token (rule 6: do not loop). Presence (waking the service when the dial is near) is not in this version.
 */
class BridgeService : Service() {

    private val main = Handler(Looper.getMainLooper())
    private var dialLink: DialLink? = null
    private var pump: Pump? = null
    private val scheduler = Scheduler { delayMs, task ->
        main.postDelayed(task, delayMs)
        Cancelable { main.removeCallbacks(task) }
    }
    private val bleRetry = Reconnector(ReconnectLadder(random = java.util.Random()), scheduler) { begin() }
    private val tunnelRetry = Reconnector(ReconnectLadder(random = java.util.Random()), scheduler) { begin() }
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
                bleRetry.stop()
                tunnelRetry.stop()
                teardown()
                BridgeState.set(Phase.IDLE, "stopped")
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            else -> {
                bleRetry.resume()
                tunnelRetry.resume()
                begin()
            }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        bleRetry.stop()
        tunnelRetry.stop()
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
                // Also ends a running session (link lost). The dial may be asleep or out of range: try again later.
                main.post { finish(mySession, Phase.FAILED, detail, bleRetry) }
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
        bleRetry.connected()
        update(mySession, Phase.CHANNEL_OPEN, "channel open, waiting for the dial")
        val p = Pump(channel, TunnelLink(BuildConfig.TUNNEL_URL), object : PumpListener {
            override fun onTunnelOpening() = update(mySession, Phase.RELAYING, "tunnel opening")
            override fun onTunnelOpen() {
                tunnelRetry.connected()
                update(mySession, Phase.RELAYING, "tunnel up")
            }
            override fun onClosed(reason: CloseReason) {
                val http = pump?.lastHttpCode()
                main.post { sessionEnded(mySession, reason, http) }
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

    private fun sessionEnded(mySession: Int, reason: CloseReason, http: Int?) {
        when {
            ReconnectPolicy.isTokenRejected(http) ->
                finish(mySession, Phase.TOKEN_REJECTED, "the tunnel refused the service token ($http). Check it in Settings.")
            !ReconnectPolicy.shouldRetry(reason, http) -> finish(mySession, Phase.CLOSED, "stopped")
            // The dial closed its side: a BLE matter. Anything else is the tunnel.
            reason == CloseReason.DIAL_CLOSED ->
                finish(mySession, Phase.CLOSED, "session ended: dial closed", bleRetry)
            else -> finish(mySession, Phase.CLOSED, "session ended: ${reason.name.lowercase()}", tunnelRetry)
        }
    }

    /**
     * End the session, keeping the last text on the status screen. With a `retry` that accepts, the service stays in
     * the foreground and starts again after the next ladder step; otherwise it stops.
     */
    private fun finish(mySession: Int, phase: Phase, detail: String, retry: Reconnector? = null) {
        if (mySession != session) return
        session++ // callbacks that teardown() triggers (pump closed, link closed) belong to the old session
        val up = pump?.bytesToTunnel() ?: 0
        val down = pump?.bytesToDial() ?: 0
        Log.i(TAG, "finish: $phase")
        teardown()
        if (retry != null && retry.failed()) {
            BridgeState.set(Phase.WAITING_RETRY, "$detail; trying again soon", up, down)
            updateNotification("Waiting to reconnect")
            return
        }
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
