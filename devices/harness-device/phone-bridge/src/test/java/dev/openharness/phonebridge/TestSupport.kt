package dev.openharness.phonebridge

import java.io.ByteArrayOutputStream
import java.util.Random
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/** Fake time: sleeping advances the clock at once, so a 4 s stall test takes microseconds and is deterministic. */
class FakeClock(start: Long = 0) : Clock {
    private val now = AtomicLong(start)
    @Volatile var onSleep: ((nowMs: Long) -> Unit)? = null
    override fun nowMs(): Long = now.get()
    override fun sleepMs(ms: Long) {
        val t = now.addAndGet(ms)
        onSleep?.invoke(t)
    }
}

/** The dial side of the pump: bytes the dial "sends" are queued by the test; bytes the pump writes are recorded. */
class FakeDial : DialChannel {
    private val incoming = LinkedBlockingQueue<ByteArray>()
    private val eof = ByteArray(0)
    private var pending: ByteArray? = null
    private var pendingPos = 0
    val written = ByteArrayOutputStream()
    val bytesRead = AtomicLong(0)
    val closeCount = AtomicInteger(0)
    private val closed = AtomicBoolean(false)

    /** While set, write() blocks (a stalled CoC: the dial has no credits). */
    @Volatile var writeBlocked = false

    fun feed(b: ByteArray) { incoming.put(b.copyOf()) }
    fun feedEof() { incoming.put(eof) }

    override fun read(buf: ByteArray): Int {
        while (true) {
            if (closed.get()) return -1
            var p = pending
            if (p == null) {
                p = incoming.poll(20, TimeUnit.MILLISECONDS) ?: continue
                if (p === eof) return -1
                pending = p
                pendingPos = 0
            }
            val n = minOf(buf.size, p.size - pendingPos)
            System.arraycopy(p, pendingPos, buf, 0, n)
            pendingPos += n
            if (pendingPos >= p.size) pending = null
            bytesRead.addAndGet(n.toLong())
            return n
        }
    }

    override fun write(buf: ByteArray, off: Int, len: Int) {
        while (writeBlocked && !closed.get()) Thread.sleep(2)
        if (closed.get()) throw java.io.IOException("closed")
        synchronized(written) { written.write(buf, off, len) }
    }

    override fun close() {
        closeCount.incrementAndGet()
        closed.set(true)
    }

    fun writtenBytes(): ByteArray = synchronized(written) { written.toByteArray() }
}

/** The tunnel side: records what the pump sends, exposes the events handle the pump gave to open(). */
class FakeTunnel : TunnelOpener {
    val openCount = AtomicInteger(0)
    @Volatile var events: TunnelEvents? = null
    val sent = ByteArrayOutputStream()
    val sendCalls = AtomicInteger(0)
    val maxSend = AtomicInteger(0)
    val closeCodes: MutableList<Int> = java.util.Collections.synchronizedList(mutableListOf<Int>())

    /** What queueSize() reports. Set it high to simulate a tunnel that does not drain. */
    @Volatile var reportedQueue = 0L
    @Volatile var sendResult = true

    override fun open(events: TunnelEvents): TunnelChannel {
        openCount.incrementAndGet()
        this.events = events
        return object : TunnelChannel {
            override fun send(bytes: ByteArray): Boolean {
                if (!sendResult) return false
                sendCalls.incrementAndGet()
                maxSend.accumulateAndGet(bytes.size) { a, b -> maxOf(a, b) }
                synchronized(sent) { sent.write(bytes) }
                return true
            }

            override fun queueSize(): Long = reportedQueue
            override fun close(code: Int) { closeCodes.add(code) }
        }
    }

    fun sentBytes(): ByteArray = synchronized(sent) { sent.toByteArray() }
}

class RecordingListener : PumpListener {
    val reasons: MutableList<CloseReason> = java.util.Collections.synchronizedList(mutableListOf<CloseReason>())
    override fun onClosed(reason: CloseReason) { reasons.add(reason) }
}

fun waitUntil(timeoutMs: Long = 10_000, what: String = "condition", cond: () -> Boolean) {
    val end = System.nanoTime() + timeoutMs * 1_000_000
    while (!cond()) {
        if (System.nanoTime() > end) throw AssertionError("timed out waiting for $what")
        Thread.sleep(2)
    }
}

/** Random bytes with `A5 48` (the cable frame magic) sprinkled in, so a pump that parsed would trip. */
fun randomPayload(seed: Long, size: Int): ByteArray {
    val rnd = Random(seed)
    val b = ByteArray(size)
    rnd.nextBytes(b)
    var i = 0
    while (i + 1 < size) {
        b[i] = 0xA5.toByte()
        b[i + 1] = 0x48
        i += 1 + rnd.nextInt(2000)
    }
    return b
}

fun chunks(data: ByteArray, seed: Long, maxChunk: Int): List<ByteArray> {
    val rnd = Random(seed)
    val out = mutableListOf<ByteArray>()
    var pos = 0
    while (pos < data.size) {
        val n = minOf(1 + rnd.nextInt(maxChunk), data.size - pos)
        out.add(data.copyOfRange(pos, pos + n))
        pos += n
    }
    return out
}
