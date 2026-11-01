package dev.openharness.phonebridge

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothSocket
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
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
 * builds pass false and use the secure call.
 */
@SuppressLint("MissingPermission") // The caller checks BLUETOOTH_SCAN and BLUETOOTH_CONNECT before start().
class DialLink(
    private val context: Context,
    private val allowUnbonded: Boolean,
    private val listener: Listener,
) {
    interface Listener {
        fun onScanning()
        fun onConnecting(name: String)
        fun onChannel(channel: DialChannel)
        fun onFailed(detail: String)
    }

    private val stopped = AtomicBoolean(false)
    private val found = AtomicBoolean(false)
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var scanCallback: ScanCallback? = null

    fun start() {
        val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            listener.onFailed("Bluetooth scanner unavailable")
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Wire.SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.scanRecord?.deviceName
                if (!Wire.isDialName(name) || !found.compareAndSet(false, true)) return
                scanner.stopScan(this)
                listener.onConnecting(name!!)
                gatt = result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            }

            override fun onScanFailed(errorCode: Int) {
                listener.onFailed("scan failed, code $errorCode")
            }
        }
        scanCallback = cb
        listener.onScanning()
        scanner.startScan(listOf(filter), settings, cb)
    }

    fun stop() {
        if (!stopped.compareAndSet(false, true)) return
        try {
            val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            scanCallback?.let { adapter?.bluetoothLeScanner?.stopScan(it) }
        } catch (_: Exception) {
        }
        try {
            gatt?.close()
        } catch (_: Exception) {
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED && !stopped.get()) {
                listener.onFailed("link lost, status $status")
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            Thread({ openChannel(g.device) }, "dial-coc-connect").start()
        }
    }

    private fun openChannel(device: BluetoothDevice) {
        try {
            val socket = if (allowUnbonded) device.createInsecureL2capChannel(Wire.PSM) else device.createL2capChannel(Wire.PSM)
            socket.connect()
            if (stopped.get()) {
                socket.close()
                return
            }
            listener.onChannel(SocketDialChannel(socket))
        } catch (e: Exception) {
            // Class name only: messages can carry device addresses.
            Log.w(TAG, "channel open failed: ${e.javaClass.simpleName}")
            listener.onFailed("channel open failed: ${e.javaClass.simpleName}")
        }
    }

    companion object {
        private const val TAG = "DialLink"
    }
}
