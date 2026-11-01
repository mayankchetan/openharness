package dev.openharness.phonebridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream

class BackpressureTest {

    private val cap = Wire.RELAY_QUEUE_BYTES.toInt()

    @Test
    fun wireConstantsAreTheSpecValues() {
        assertEquals(65536, cap)
        assertEquals(4000L, Wire.RELAY_FULL_CLOSE_MS)
    }

    @Test
    fun queueNeverExceedsItsCapacityUnderASlowConsumer() {
        val q = ByteQueue(cap, RealClock, Wire.RELAY_FULL_CLOSE_MS)
        val data = randomPayload(21, 1_000_000)
        val got = ByteArrayOutputStream()
        val consumer = Thread {
            val buf = ByteArray(3000)
            while (true) {
                val n = q.take(buf)
                if (n < 0) break
                got.write(buf, 0, n)
                if (got.size() % 9000 < 3000) Thread.sleep(1) // slow sink
            }
        }
        consumer.start()
        for (c in chunks(data, 22, 20_000)) assertEquals(PutResult.OK, q.put(c))
        q.close()
        consumer.join(20_000)
        assertArrayEquals(data, got.toByteArray())
        assertTrue("high water ${q.highWater()} must stay within $cap", q.highWater() <= cap)
        assertTrue("the test must actually fill the queue", q.highWater() >= cap - 20_000)
    }

    @Test
    fun aQueueFullForFourSecondsStalls() {
        val clock = FakeClock()
        val q = ByteQueue(cap, clock, Wire.RELAY_FULL_CLOSE_MS)
        assertEquals(PutResult.OK, q.put(ByteArray(cap)))
        val t0 = clock.nowMs()
        assertEquals(PutResult.STALLED, q.put(ByteArray(10)))
        val waited = clock.nowMs() - t0
        assertTrue("stalled after $waited ms, want >= 4000", waited >= 4000)
        assertTrue("stalled after $waited ms, want < 4100", waited < 4100)
        assertEquals("a stalled put adds nothing", cap, q.size())
    }

    @Test
    fun aDrainAt3900msSavesTheSession() {
        val clock = FakeClock()
        val q = ByteQueue(cap, clock, Wire.RELAY_FULL_CLOSE_MS)
        assertEquals(PutResult.OK, q.put(ByteArray(cap)))
        val t0 = clock.nowMs()
        var drained = false
        clock.onSleep = { now ->
            if (!drained && now - t0 >= 3900) {
                drained = true
                q.take(ByteArray(cap))
            }
        }
        assertEquals(PutResult.OK, q.put(ByteArray(1000)))
        assertTrue(drained)
        assertEquals(1000, q.size())
    }

    @Test
    fun aPartialPutThatMakesProgressDoesNotStall() {
        // A consumer that frees 100 bytes every 500 ms never lets the queue stay full for 4 s.
        val clock = FakeClock()
        val q = ByteQueue(cap, clock, Wire.RELAY_FULL_CLOSE_MS)
        assertEquals(PutResult.OK, q.put(ByteArray(cap)))
        var last = 0L
        clock.onSleep = { now ->
            if (now - last >= 500) {
                last = now
                q.take(ByteArray(100))
            }
        }
        assertEquals(PutResult.OK, q.put(ByteArray(2000)))
    }

    @Test
    fun abortWakesABlockedProducer() {
        val q = ByteQueue(cap, RealClock, Wire.RELAY_FULL_CLOSE_MS)
        assertEquals(PutResult.OK, q.put(ByteArray(cap)))
        var result: PutResult? = null
        val t = Thread { result = q.put(ByteArray(10)) }
        t.start()
        Thread.sleep(50)
        q.abort()
        t.join(2000)
        assertEquals(PutResult.CLOSED, result)
    }

    @Test
    fun tunnelThatDoesNotDrainStopsReadingTheDialThenClosesBothSides() {
        val clock = FakeClock()
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        tunnel.reportedQueue = 10_000_000 // never drains
        val listener = RecordingListener()
        val pump = Pump(dial, tunnel, listener, clock = clock)
        pump.start()
        val fed = 400_000
        chunks(randomPayload(31, fed), 32, 5000).forEach { dial.feed(it) }
        waitUntil(what = "stall close") { listener.reasons.isNotEmpty() }
        assertEquals(listOf(CloseReason.STALL_TO_TUNNEL), listener.reasons.toList())
        assertEquals("both sides closed", 1, dial.closeCount.get())
        assertEquals(listOf(1000), tunnel.closeCodes.toList())
        val read = dial.bytesRead.get()
        val most = cap + 2L * Pump.CHUNK_BYTES
        assertTrue("read $read bytes of $fed; the pump must stop reading near $most", read <= most)
        assertEquals("nothing was sent while the tunnel was backed up", 0, tunnel.sendCalls.get())
        pump.stop()
    }

    @Test
    fun dialThatDoesNotAcceptStopsTheTunnelThenClosesBothSides() {
        val clock = FakeClock()
        val dial = FakeDial()
        dial.writeBlocked = true
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = Pump(dial, tunnel, listener, clock = clock)
        pump.start()
        dial.feed(byteArrayOf(1))
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        val events = tunnel.events!!
        val flood = Thread { chunks(randomPayload(41, 400_000), 42, 5000).forEach { events.onBinary(it) } }
        flood.start()
        waitUntil(what = "stall close") { listener.reasons.isNotEmpty() }
        flood.join(5000)
        assertEquals(listOf(CloseReason.STALL_TO_DIAL), listener.reasons.toList())
        assertEquals(1, dial.closeCount.get())
        assertEquals(listOf(1000), tunnel.closeCodes.toList())
        pump.stop()
    }
}
