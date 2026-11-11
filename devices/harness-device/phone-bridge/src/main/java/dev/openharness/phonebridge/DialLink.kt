package dev.openharness.phonebridge

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothSocket
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean

/** The L2CAP channel as a DialChannel. read() returns -1 at end of stream. */
class SocketDialChannel(private val socket: BluetoothSocket) : DialChannel {
    private val input = socket.inputStream
    private val output = socket.outputStream
    override fun read(buf: ByteArray): Int = input.read(buf)
    override fun write(buf: ByteArray, off: Int, len: Int) = output.write(buf, off, len)
    override fun close() {
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }
}

/**
 * Finds the dial, connects, opens the CoC and hands it over. Same sequence as the bench app: scan, connectGatt on
 * LE, discover services, then the channel on the fixed PSM (the spec says the fixed value is the contract, so the PSM
 * characteristic is not read).
 *
 * `allowUnbonded` is the dev flag: it uses the insecure channel call so M1 can run before pairing exists. Product
 * builds pass false and use the secure call. Before that call a product build bonds with the dial (BondGate): the
 * secure call alone does not start a pairing that survives the retry ladder, so the app calls createBond() and waits
 * for the bond broadcast, up to BondGate.TIMEOUT_MS. It never tears the link down while Android is bonding.
 *
 * The listener may be called on any thread. After stop() no callback reports a failure.
 */
@SuppressLint("MissingPermission") // The caller checks BLUETOOTH_SCAN and BLUETOOTH_CONNECT before start().
class DialLink(
    private val context: Context,
    private val allowUnbonded: Boolean,
    private val listener: Listener,
    /** Owned by the service, not by this link: every retry of the ladder makes a new DialLink. */
    private val staleBonds: StaleBondCounter,
) {
    interface Listener {
        fun onScanning()
        fun onConnecting(name: String)

        /** The dial is not bonded yet: Android is pairing and the user must confirm the number. */
        fun onPairing() {}
        fun onChannel(channel: DialChannel)

        /** The link failed. `kind` tells the caller whether the retry ladder may run again. */
        fun onFailed(detail: String, kind: FailureKind)
    }

    private val stopped = AtomicBoolean(false)
    private val found = AtomicBoolean(false)
    private val channelStarted = AtomicBoolean(false)

    /** True once the current channel attempt has opened. Cleared when an attempt starts (startAttempt). */
    @Volatile private var channelUp = false

    /** One attempt counts as one failure, even when the socket and the GATT link both report it. */
    private val attemptFailed = AtomicBoolean(false)
    private val main = Handler(Looper.getMainLooper())

    @Volatile private var gatt: BluetoothGatt? = null

    /** Set once close() ran on the GATT: no status write can start after that. */
    private val gattClosed = AtomicBoolean(false)

    /** The token of our status writer on the shared publisher, so a late detach cannot drop a newer link's writer. */
    @Volatile private var statusToken: Any? = null
    @Volatile private var scanCallback: ScanCallback? = null

    private val bondGate = BondGate(allowUnbonded)
    private val bondGateStarted = AtomicBoolean(false)
    private val bondReceiverOn = AtomicBoolean(false)
    private var bondReceiver: BroadcastReceiver? = null
    private val bondTimeout = Runnable { handleBondStep(bondGate.onTimeout()) }

    /** The socket while connect() blocks, so stop() can close it and unblock that thread. */
    @Volatile private var connecting: BluetoothSocket? = null

    private val scanTimeout = Runnable {
        if (!stopped.get() && !found.get()) {
            stopScan()
            listener.onFailed("dial not found", FailureKind.LINK)
        }
    }

    fun start() {
        val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            listener.onFailed("Bluetooth scanner unavailable", FailureKind.LINK)
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Wire.SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                if (stopped.get()) return
                val name = result.scanRecord?.deviceName
                if (!Wire.isDialName(name) || !found.compareAndSet(false, true)) return
                main.removeCallbacks(scanTimeout)
                scanner.stopScan(this)
                listener.onConnecting(name!!)
                val g = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
                gatt = g
                // stop() may have run between the check above and the assignment: it saw no gatt, so close it here.
                if (stopped.get()) g.close()
            }

            override fun onScanFailed(errorCode: Int) {
                if (!stopped.get()) listener.onFailed("scan failed, code $errorCode", FailureKind.LINK)
            }
        }
        scanCallback = cb
        listener.onScanning()
        scanner.startScan(listOf(filter), settings, cb)
        main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS)
    }

    fun stop() {
        if (!stopped.compareAndSet(false, true)) return
        main.removeCallbacks(scanTimeout)
        endBondWait()
        stopScan()
        try {
            connecting?.close() // unblocks connect() on its thread
        } catch (_: Exception) {
        }
        val g = gatt ?: return
        val publisher = StatusPublisher.shared
        val token = statusToken
        if (publisher == null || token == null) {
            closeGatt(g)
            return
        }
        // The last status (token rejected, no network) may still be on its way to the dial: let that write finish before
        // the link goes down, but wait at most STATUS_DRAIN_MS. Off the main thread, so the caller is not held.
        Thread({
            publisher.awaitIdle(token, STATUS_DRAIN_MS)
            publisher.detach(token)
            closeGatt(g)
        }, "dial-status-drain").start()
    }

    private fun closeGatt(g: BluetoothGatt) {
        gattClosed.set(true)
        try {
            g.close()
        } catch (_: Exception) {
        }
    }

    /**
     * Opens a new CoC on the GATT link that is still up (spec 6.3 rule 4: a tunnel error closes only the channel).
     * False if the link is gone or stopped; the caller then starts over with a scan.
     */
    fun reopenChannel(): Boolean {
        val g = gatt
        if (stopped.get() || g == null || gattClosed.get() || !channelStarted.get()) return false
        startAttempt()
        Thread({ openChannel(g.device) }, "dial-coc-reconnect").start()
        return true
    }

    private fun stopScan() {
        try {
            val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            scanCallback?.let { adapter?.bluetoothLeScanner?.stopScan(it) }
        } catch (_: Exception) {
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                statusToken?.let { StatusPublisher.shared?.detach(it) }
                if (stopped.get()) return
                val detail = "link lost, status $status"
                // The dial dropping the link while the channel is still being set up (for example right after the
                // encryption fails) counts like a failed channel attempt. A drop after the channel opened does not.
                if (channelStarted.get() && !channelUp) channelFailed(detail, g.device)
                else main.post { linkLost(detail, g) }
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (characteristic.uuid == Wire.STATUS_UUID) {
                if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "status write failed, code $status")
                StatusPublisher.shared?.onWriteDone()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (stopped.get()) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onFailed("service discovery failed, status $status", FailureKind.LINK)
                return
            }
            if (!bondGateStarted.compareAndSet(false, true)) return
            // The dev build has nothing to wait for: it runs inline, as before. A product build moves to the main looper,
            // where the bond receiver and the timer live.
            if (allowUnbonded) beginBondGate(g) else main.post { beginBondGate(g) }
        }
    }

    /** Main thread. Registers the bond receiver first, then reads the state, so no broadcast is missed. */
    private fun beginBondGate(g: BluetoothGatt) {
        if (stopped.get()) return
        if (!allowUnbonded) registerBondReceiver(g.device)
        val step = bondGate.start(BondState.fromAndroid(g.device.bondState))
        if (step == BondStep.StartBond || step == BondStep.Wait) main.postDelayed(bondTimeout, BondGate.TIMEOUT_MS)
        handleBondStep(step, g)
    }

    private fun handleBondStep(step: BondStep, g: BluetoothGatt? = gatt) {
        if (stopped.get() || g == null) return
        when (step) {
            BondStep.OpenChannel -> {
                endBondWait()
                proceedToChannel(g)
            }
            BondStep.StartBond -> {
                listener.onPairing()
                val started = try {
                    g.device.createBond()
                } catch (e: Exception) {
                    Log.w(TAG, "createBond refused: ${e.javaClass.simpleName}")
                    false
                }
                if (!started) handleBondStep(bondGate.onCreateBondRefused(), g)
            }
            BondStep.Wait -> listener.onPairing()
            is BondStep.Fail -> {
                endBondWait()
                Log.w(TAG, "pairing failed")
                listener.onFailed(step.detail, FailureKind.PAIRING)
            }
        }
    }

    /** The bond is there (or the dev build skips it): open the channel and attach the status writer. */
    private fun proceedToChannel(g: BluetoothGatt) {
        if (channelStarted.compareAndSet(false, true)) {
            startAttempt()
            Thread({ openChannel(g.device) }, "dial-coc-connect").start()
        }
        // Spec 6.3 rule 8. The characteristic needs an encrypted link, so a dev build that skips pairing writes only to a
        // dial that is already bonded: a write to an unbonded dial would start a pairing the user did not ask for. A
        // product build gets here only once bonded.
        if (!allowUnbonded || g.device.bondState == BluetoothDevice.BOND_BONDED) {
            statusToken = StatusPublisher.shared?.attach { writeStatus(g, it) }
        }
    }

    private fun registerBondReceiver(device: BluetoothDevice) {
        if (!bondReceiverOn.compareAndSet(false, true)) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                val who = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                }
                if (who?.address != device.address) return
                val state = BondState.fromAndroid(intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE))
                handleBondStep(bondGate.onBondChanged(state))
            }
        }
        bondReceiver = r
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        // Exported on purpose. ACTION_BOND_STATE_CHANGED is a protected broadcast: only the Bluetooth stack (a core
        // uid, not the app's own uid) can send it, so no third-party app can forge it. A NOT_EXPORTED receiver would
        // get it only from its own app or from the root and system uids, so the bond state would never arrive. The
        // receiver still checks the action and the device address before it acts.
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(r, filter, Context.RECEIVER_EXPORTED)
        else context.registerReceiver(r, filter)
    }

    /** Stops waiting for a bond: no timer, no receiver. Safe to call twice and from any thread. */
    private fun endBondWait() {
        main.removeCallbacks(bondTimeout)
        if (!bondReceiverOn.compareAndSet(true, false)) return
        try {
            bondReceiver?.let { context.unregisterReceiver(it) }
        } catch (_: Exception) {
        }
        bondReceiver = null
    }

    /** One write of the relay-status byte. Returns true when the write is out and onCharacteristicWrite will follow. */
    private fun writeStatus(g: BluetoothGatt, status: RelayStatus): Boolean {
        if (gattClosed.get()) return false
        val ch = g.getService(Wire.SERVICE_UUID)?.getCharacteristic(Wire.STATUS_UUID) ?: return false
        val value = byteArrayOf(status.code)
        return try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(ch, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    ch.value = value
                    g.writeCharacteristic(ch)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "status write refused: ${e.javaClass.simpleName}")
            false
        }
    }

    private fun openChannel(device: BluetoothDevice) {
        var socket: BluetoothSocket? = null
        try {
            socket = if (allowUnbonded) device.createInsecureL2capChannel(Wire.PSM) else device.createL2capChannel(Wire.PSM)
            connecting = socket
            socket.connect()
            connecting = null
            if (stopped.get()) {
                socket.close()
                return
            }
            channelUp = true
            staleBonds.onChannelOpened()
            listener.onChannel(SocketDialChannel(socket))
        } catch (e: Exception) {
            connecting = null
            try {
                socket?.close()
            } catch (_: Exception) {
            }
            if (stopped.get()) return // stop() closed the socket on purpose
            // Class name only: messages can carry device addresses.
            Log.w(TAG, "channel open failed: ${e.javaClass.simpleName}")
            channelFailed("channel open failed: ${e.javaClass.simpleName}", device)
        }
    }

    /**
     * Main thread. The link dropped before any channel was up. A pairing still waiting for an answer has failed, and it
     * stops like any failed pairing: a retry would prompt both devices again. Otherwise the ladder runs as before.
     */
    private fun linkLost(detail: String, g: BluetoothGatt) {
        if (stopped.get()) return
        val step = bondGate.onLinkLost()
        if (step is BondStep.Fail) handleBondStep(step, g) else listener.onFailed(detail, FailureKind.LINK)
    }

    /** A new channel attempt begins (first open, or a tunnel retry on the kept link). */
    private fun startAttempt() {
        channelUp = false
        attemptFailed.set(false)
    }

    /**
     * A channel attempt failed. Counted once per attempt. Android reads the bond for the dial now: a bonded dial that
     * keeps failing is a stale bond (the dial forgot this phone), and the counter says so after two in a row.
     */
    private fun channelFailed(detail: String, device: BluetoothDevice) {
        if (stopped.get() || !attemptFailed.compareAndSet(false, true)) return
        val kind = staleBonds.onChannelFailed(device.bondState == BluetoothDevice.BOND_BONDED)
        listener.onFailed(if (kind == FailureKind.STALE_BOND) StaleBondCounter.STALE_TEXT else detail, kind)
    }

    companion object {
        /** How long to scan before giving up with "dial not found". */
        const val SCAN_TIMEOUT_MS = 30_000L

        /** How long stop() lets the last relay-status write finish before it closes the GATT link. */
        const val STATUS_DRAIN_MS = 300L
        private const val TAG = "DialLink"
    }
}
