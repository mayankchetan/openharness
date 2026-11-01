package dev.openharness.phonebridge

import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

enum class PutResult { OK, STALLED, CLOSED }

/**
 * Bounded byte FIFO with one producer side and one consumer side.
 *
 * put() blocks while the queue is full. Blocking is the backpressure: the caller stops reading its source. If the
 * queue stays full for `fullCloseMs` without any room appearing, put() returns STALLED and the caller closes both
 * sides (TLS cannot resync after a gap). The size never exceeds `capacity`.
 *
 * close() lets the consumer drain what is queued, then take() returns -1. abort() drops the queue at once.
 */
class ByteQueue(val capacity: Int, private val clock: Clock, private val fullCloseMs: Long) {
    private val lock = ReentrantLock()
    private val dataReady = lock.newCondition()
    private val buf = ByteArray(capacity)
    private var head = 0
    private var size = 0
    private var high = 0
    private var closed = false
    private var aborted = false

    fun size(): Int = lock.withLock { size }
    fun highWater(): Int = lock.withLock { high }

    fun put(src: ByteArray, off: Int = 0, len: Int = src.size): PutResult {
        var pos = off
        var left = len
        var blocked = false // not a negative number: System.nanoTime() may be negative
        var blockedSince = 0L
        while (true) {
            lock.withLock {
                if (closed || aborted) return PutResult.CLOSED
                val n = minOf(capacity - size, left)
                if (n > 0) {
                    var tail = (head + size) % capacity
                    var rest = n
                    var p = pos
                    while (rest > 0) {
                        val run = minOf(rest, capacity - tail)
                        System.arraycopy(src, p, buf, tail, run)
                        tail = (tail + run) % capacity
                        p += run
                        rest -= run
                    }
                    size += n
                    if (size > high) high = size
                    pos += n
                    left -= n
                    blocked = false // room appeared, so the queue was not full for this whole time
                    dataReady.signalAll()
                }
            }
            if (left == 0) return PutResult.OK
            val now = clock.nowMs()
            if (!blocked) {
                blocked = true
                blockedSince = now
            } else if (now - blockedSince >= fullCloseMs) {
                return PutResult.STALLED
            }
            clock.sleepMs(POLL_MS)
        }
    }

    /** Blocks until data is available. Returns the byte count, or -1 when closed and drained, or aborted. */
    fun take(dst: ByteArray): Int {
        lock.withLock {
            while (true) {
                if (aborted) return -1
                if (size > 0) {
                    val n = minOf(size, dst.size)
                    var rest = n
                    var d = 0
                    while (rest > 0) {
                        val run = minOf(rest, capacity - head)
                        System.arraycopy(buf, head, dst, d, run)
                        head = (head + run) % capacity
                        d += run
                        rest -= run
                    }
                    size -= n
                    return n
                }
                if (closed) return -1
                dataReady.await()
            }
        }
    }

    fun close() = lock.withLock {
        closed = true
        dataReady.signalAll()
    }

    fun abort() = lock.withLock {
        aborted = true
        closed = true
        size = 0
        dataReady.signalAll()
    }

    companion object {
        const val POLL_MS = 5L
    }
}
