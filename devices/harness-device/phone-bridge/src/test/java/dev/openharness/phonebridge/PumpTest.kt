package dev.openharness.phonebridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PumpTest {

    private fun newPump(dial: FakeDial, tunnel: FakeTunnel, listener: PumpListener = RecordingListener(), clock: Clock = RealClock) =
        Pump(dial, tunnel, listener, clock = clock)

    @Test
    fun oneMegabyteBothWaysArrivesEqualAndInOrder() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val pump = newPump(dial, tunnel)
        pump.start()

        val up = randomPayload(1, 1_000_000)     // dial -> tunnel
        val down = randomPayload(2, 1_000_000)   // tunnel -> dial
        val feeder = Thread { chunks(up, 11, 700).forEach { dial.feed(it) } }
        feeder.start()
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        val events = tunnel.events!!
        val sender = Thread { chunks(down, 12, 1500).forEach { events.onBinary(it) } }
        sender.start()

        waitUntil(what = "all upstream bytes") { tunnel.sentBytes().size >= up.size }
        waitUntil(what = "all downstream bytes") { dial.writtenBytes().size >= down.size }
        feeder.join()
        sender.join()

        assertArrayEquals(up, tunnel.sentBytes())
        assertArrayEquals(down, dial.writtenBytes())
        assertEquals(up.size.toLong(), pump.bytesToTunnel())
        assertEquals(down.size.toLong(), pump.bytesToDial())
        pump.stop()
    }

    @Test
    fun tunnelOpensOnlyOnTheFirstDialByte() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val pump = newPump(dial, tunnel)
        pump.start()
        Thread.sleep(150)
        assertEquals("no byte yet, so no WebSocket", 0, tunnel.openCount.get())
        dial.feed(byteArrayOf(0x16, 0x03, 0x03))
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        dial.feed(byteArrayOf(1, 2, 3))
        waitUntil(what = "bytes sent") { tunnel.sentBytes().size == 6 }
        assertEquals("one WebSocket for the whole session", 1, tunnel.openCount.get())
        pump.stop()
    }

    @Test
    fun dialCloseDeliversPendingBytesThenClosesTunnelWithCode1000Once() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        val data = randomPayload(3, 50_000)
        chunks(data, 5, 900).forEach { dial.feed(it) }
        dial.feedEof()
        waitUntil(what = "tunnel closed") { tunnel.closeCodes.isNotEmpty() }
        waitUntil(what = "listener") { listener.reasons.isNotEmpty() }
        Thread.sleep(50)
        assertArrayEquals("bytes queued before the close still arrive", data, tunnel.sentBytes())
        assertEquals(listOf(1000), tunnel.closeCodes.toList())
        assertEquals(listOf(CloseReason.DIAL_CLOSED), listener.reasons.toList())
        pump.stop()
        assertEquals("stop after close adds nothing", 1, listener.reasons.size)
    }

    @Test
    fun dialCloseBeforeAnyByteNeverOpensTunnel() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        dial.feedEof()
        waitUntil(what = "listener") { listener.reasons.isNotEmpty() }
        assertEquals(0, tunnel.openCount.get())
        assertTrue(tunnel.closeCodes.isEmpty())
    }

    @Test
    fun tunnelFailureClosesTheDialChannelOnce() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        dial.feed(byteArrayOf(1))
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        tunnel.events!!.onFailure(403, RuntimeException("edge said no"))
        waitUntil(what = "listener") { listener.reasons.isNotEmpty() }
        assertEquals(listOf(CloseReason.TUNNEL_FAILED), listener.reasons.toList())
        assertEquals(403, pump.lastHttpCode())
        assertEquals(1, dial.closeCount.get())
        pump.stop()
        assertEquals(1, dial.closeCount.get())
        assertEquals(1, listener.reasons.size)
    }

    @Test
    fun tunnelClosedByPeerClosesTheDialChannel() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        dial.feed(byteArrayOf(1))
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        tunnel.events!!.onClosed(1001)
        waitUntil(what = "listener") { listener.reasons.isNotEmpty() }
        assertEquals(listOf(CloseReason.TUNNEL_CLOSED), listener.reasons.toList())
        assertEquals(1, dial.closeCount.get())
        pump.stop()
    }

    @Test
    fun sendRefusedByTheTunnelCountsAsFailure() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        tunnel.sendResult = false
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        dial.feed(byteArrayOf(9, 9, 9))
        waitUntil(what = "listener") { listener.reasons.isNotEmpty() }
        assertEquals(listOf(CloseReason.TUNNEL_FAILED), listener.reasons.toList())
        assertEquals(1, dial.closeCount.get())
        pump.stop()
    }

    @Test
    fun stopClosesBothSidesOnceAndReportsStopped() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val listener = RecordingListener()
        val pump = newPump(dial, tunnel, listener)
        pump.start()
        dial.feed(byteArrayOf(1))
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        pump.stop()
        pump.stop()
        assertEquals(listOf(CloseReason.STOPPED), listener.reasons.toList())
        assertEquals(1, dial.closeCount.get())
        assertEquals(listOf(1000), tunnel.closeCodes.toList())
    }
}
