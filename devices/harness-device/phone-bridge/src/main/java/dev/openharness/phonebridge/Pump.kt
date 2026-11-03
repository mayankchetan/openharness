package dev.openharness.phonebridge

import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/** The dial side: a stream of bytes (the BLE L2CAP channel). read() blocks; -1 means the channel closed. */
interface DialChannel {
    fun read(buf: ByteArray): Int
    fun write(buf: ByteArray, off: Int, len: Int)
    fun close()
}

/** The daemon side: a WebSocket that carries binary messages. */
interface TunnelChannel {
    /** False if the socket refused the message (closed). */
    fun send(bytes: ByteArray): Boolean

    /** Bytes accepted by send() and not yet written to the network. */
    fun queueSize(): Long
    fun close(code: Int)
}

interface TunnelOpener {
    fun open(events: TunnelEvents): TunnelChannel
}

/** Callbacks from the tunnel. They may arrive on any thread. onBinary may block: that is the backpressure. */
interface TunnelEvents {
    fun onOpen()
    fun onBinary(bytes: ByteArray)
    fun onClosed(code: Int)

    /** `httpCode` is the HTTP status when the upgrade was refused (for example 401 or 403 from the edge). */
    fun onFailure(httpCode: Int?, cause: Throwable)
}

enum class CloseReason { DIAL_CLOSED, TUNNEL_CLOSED, TUNNEL_FAILED, STALL_TO_TUNNEL, STALL_TO_DIAL, STOPPED }

interface PumpListener {
    fun onTunnelOpening() {}
    fun onTunnelOpen() {}

    /** Called once, from whichever thread closed the pump. Both sides are already closed. */
    fun onClosed(reason: CloseReason) {}

    /** Same call, with the HTTP status of a refused upgrade (401, 403 ...) or null. Override this one if you need the code. */
    fun onClosed(reason: CloseReason, httpCode: Int?) = onClosed(reason)
}

/**
 * Copies bytes between the dial channel and the tunnel. It never looks at them (spec section 6, rule 1).
 *
 * - The tunnel opens on the first dial byte (rule 2). A dial close drains queued bytes, then closes the tunnel with 1000.
 * - Each direction has a bounded queue (rule 3). A full queue stops reading the source. Full for 4 s closes both sides.
 * - Any tunnel failure closes the dial channel. The caller decides what to do next (reconnect is B10b).
 */
class Pump(
    private val dial: DialChannel,
    private val opener: TunnelOpener,
    private val listener: PumpListener,
    private val clock: Clock = RealClock,
    queueBytes: Int = Wire.RELAY_QUEUE_BYTES.toInt(),
    private val fullCloseMs: Long = Wire.RELAY_FULL_CLOSE_MS,
    private val chunkBytes: Int = CHUNK_BYTES,
) : TunnelEvents {

    private val toTunnel = ByteQueue(queueBytes, clock, fullCloseMs)
    private val toDial = ByteQueue(queueBytes, clock, fullCloseMs)
    private val tunnelRef = AtomicReference<TunnelChannel?>(null)
    private val tunnelReady = CountDownLatch(1)
    private val dialWriterDone = CountDownLatch(1)
    private val closing = AtomicBoolean(false)
    private val started = AtomicBoolean(false)
    private val upBytes = AtomicLong(0)
    private val downBytes = AtomicLong(0)
    @Volatile private var httpCode: Int? = null

    /** Bytes read from the dial. Counted when read, before they are queued. */
    fun bytesToTunnel(): Long = upBytes.get()

    /** Bytes received from the tunnel. Counted when received, before they are queued. */
    fun bytesToDial(): Long = downBytes.get()
    fun lastHttpCode(): Int? = httpCode

    fun start() {
        check(started.compareAndSet(false, true)) { "already started" }
        thread("pump-dial-reader", ::readDial)
        thread("pump-tunnel-writer", ::writeTunnel)
        thread("pump-dial-writer", ::writeDial)
    }

    fun stop() = closeAll(CloseReason.STOPPED)

    private fun thread(name: String, body: () -> Unit) {
        Thread(body, name).apply { isDaemon = true }.start()
    }

    private fun readDial() {
        val buf = ByteArray(chunkBytes)
        try {
            while (true) {
                val n = dial.read(buf)
                if (n < 0) break
                if (n == 0) continue
                if (tunnelRef.get() == null && !openTunnel()) return
                upBytes.addAndGet(n.toLong())
                when (toTunnel.put(buf, 0, n)) {
                    PutResult.OK -> {}
                    PutResult.STALLED -> { closeAll(CloseReason.STALL_TO_TUNNEL); return }
                    PutResult.CLOSED -> return
                }
            }
        } catch (_: IOException) {
            // The channel closed under us: treat as end of stream.
        }
        if (tunnelRef.get() == null) closeAll(CloseReason.DIAL_CLOSED) else toTunnel.close()
    }

    private fun openTunnel(): Boolean {
        if (closing.get()) return false
        listener.onTunnelOpening()
        val ch = try {
            opener.open(this)
        } catch (e: Exception) {
            closeAll(CloseReason.TUNNEL_FAILED)
            return false
        }
        tunnelRef.set(ch)
        tunnelReady.countDown()
        if (closing.get()) ch.close(NORMAL_CLOSE)
        return true
    }

    private fun writeTunnel() {
        try {
            tunnelReady.await()
        } catch (_: InterruptedException) {
            return
        }
        val tunnel = tunnelRef.get() ?: return
        val buf = ByteArray(chunkBytes)
        while (true) {
            val n = toTunnel.take(buf)
            if (n < 0) break
            // One WebSocket message never exceeds the daemon's limit (Wire.MAX_WS_MESSAGE_BYTES): split, keep order.
            var off = 0
            while (off < n) {
                val len = minOf(n - off, Wire.MAX_WS_MESSAGE_BYTES)
                // Hold the piece until the WebSocket has room, so the queue in front of it is the only deep one.
                // The same 4 s rule as a full queue: a WebSocket that never drains is a stall.
                val waitStart = clock.nowMs()
                while (!closing.get() && tunnel.queueSize() >= SEND_AHEAD_BYTES) {
                    if (Thread.currentThread().isInterrupted) { closeAll(CloseReason.STOPPED); return }
                    if (clock.nowMs() - waitStart >= fullCloseMs) { closeAll(CloseReason.STALL_TO_TUNNEL); return }
                    clock.sleepMs(2)
                }
                if (closing.get()) return
                if (!tunnel.send(buf.copyOfRange(off, off + len))) {
                    closeAll(CloseReason.TUNNEL_FAILED)
                    return
                }
                off += len
            }
        }
        closeAll(CloseReason.DIAL_CLOSED) // drained after the dial closed; a no-op after any other close
    }

    private fun writeDial() {
        val buf = ByteArray(chunkBytes)
        try {
            while (true) {
                val n = toDial.take(buf)
                if (n < 0) return
                dial.write(buf, 0, n)
            }
        } catch (_: IOException) {
            closeAll(CloseReason.DIAL_CLOSED)
        } finally {
            dialWriterDone.countDown()
        }
    }

    // TunnelEvents (called by the tunnel implementation)

    override fun onOpen() = listener.onTunnelOpen()

    override fun onBinary(bytes: ByteArray) {
        if (closing.get()) return
        downBytes.addAndGet(bytes.size.toLong())
        if (toDial.put(bytes) == PutResult.STALLED) closeAll(CloseReason.STALL_TO_DIAL)
    }

    override fun onClosed(code: Int) = closeAll(CloseReason.TUNNEL_CLOSED)

    override fun onFailure(httpCode: Int?, cause: Throwable) {
        this.httpCode = httpCode
        closeAll(CloseReason.TUNNEL_FAILED)
    }

    private fun closeAll(reason: CloseReason) {
        if (!closing.compareAndSet(false, true)) return
        toTunnel.abort()
        tunnelReady.countDown()
        if (reason == CloseReason.TUNNEL_CLOSED) drainToDial() else toDial.abort()
        // A tunnel that failed or closed itself is already gone; any other reason closes it politely.
        if (reason != CloseReason.TUNNEL_FAILED && reason != CloseReason.TUNNEL_CLOSED) {
            tunnelRef.get()?.close(NORMAL_CLOSE)
        }
        try {
            dial.close()
        } catch (_: Exception) {
        }
        listener.onClosed(reason, httpCode)
    }

    /**
     * The tunnel closed in an orderly way: let what is already queued reach the dial, but wait at most DRAIN_MS, so a
     * stuck dial cannot hold the close. The dial writer ends on its own once the closed queue is empty.
     */
    private fun drainToDial() {
        toDial.close()
        val start = clock.nowMs()
        while (dialWriterDone.count > 0 && clock.nowMs() - start < DRAIN_MS) clock.sleepMs(5)
        toDial.abort()
    }

    companion object {
        const val DRAIN_MS = 1000L
        const val CHUNK_BYTES = 8192
        const val SEND_AHEAD_BYTES = 8192L
        const val NORMAL_CLOSE = 1000
    }
}
