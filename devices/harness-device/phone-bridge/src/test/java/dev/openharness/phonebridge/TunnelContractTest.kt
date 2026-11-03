package dev.openharness.phonebridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * What the daemon's relay endpoint demands of the phone (track B, B3b): binary only, at most 64 KiB a message, no
 * Origin header. Plus how TunnelLink behaves against a real WebSocket peer on loopback.
 */
class TunnelContractTest {

    private val limit = 64 * 1024

    private class Events : TunnelEvents {
        val opened = AtomicInteger(0)
        val binary = LinkedBlockingQueue<ByteArray>()
        val closed = LinkedBlockingQueue<Int>()
        val failures = LinkedBlockingQueue<Int>()
        override fun onOpen() { opened.incrementAndGet() }
        override fun onBinary(bytes: ByteArray) { binary.put(bytes) }
        override fun onClosed(code: Int) { closed.put(code) }
        override fun onFailure(httpCode: Int?, cause: Throwable) { failures.put(httpCode ?: -1) }
    }

    @Test
    fun pumpNeverSendsAMessageAbove64KiB() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        // Chunk and queue above the limit on purpose, so the cap is what splits the data.
        val pump = Pump(dial, tunnel, RecordingListener(), queueBytes = 1_048_576, chunkBytes = 300_000)
        pump.start()
        val data = randomPayload(61, 1_000_000)
        chunks(data, 62, 300_000).forEach { dial.feed(it) }
        waitUntil(what = "all bytes sent") { tunnel.sentBytes().size >= data.size }
        assertTrue("largest message ${tunnel.maxSend.get()} must be <= $limit", tunnel.maxSend.get() <= limit)
        assertTrue("sanity: the cap was reached", tunnel.maxSend.get() == limit)
        assertArrayEquals(data, tunnel.sentBytes())
        pump.stop()
    }

    @Test
    fun a200KBBurstIsSplitIntoMessagesOfAtMost64KiBInOrder() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val pump = Pump(dial, tunnel, RecordingListener(), queueBytes = 1_048_576, chunkBytes = 300_000)
        pump.start()
        val burst = randomPayload(71, 200_000)
        dial.feed(burst)
        waitUntil(what = "burst sent") { tunnel.sentBytes().size >= burst.size }
        assertTrue("largest message ${tunnel.maxSend.get()} must be <= ${Wire.MAX_WS_MESSAGE_BYTES}", tunnel.maxSend.get() <= Wire.MAX_WS_MESSAGE_BYTES)
        assertTrue("200000 bytes need at least 4 messages, got ${tunnel.sendCalls.get()}", tunnel.sendCalls.get() >= 4)
        assertArrayEquals("order preserved", burst, tunnel.sentBytes())
        pump.stop()
    }

    @Test
    fun messageCapIsExactly64KiB() {
        assertEquals(65_536, Wire.MAX_WS_MESSAGE_BYTES)
    }

    @Test
    fun tunnelLinkSetsNoOriginHeaderAndKeepsCustomHeaders() {
        TestWsServer().use { server ->
            val ev = Events()
            val ch = TunnelLink(server.url, mapOf("X-Test" to "1")).open(ev)
            assertTrue(server.awaitConnected())
            assertFalse("no Origin header: ${server.requestHeaders.keys}", server.requestHeaders.containsKey("origin"))
            assertEquals("1", server.requestHeaders["x-test"])
            ch.close(1000)
        }
    }

    @Test
    fun binaryMessagesFlowBothWays() {
        TestWsServer().use { server ->
            val ev = Events()
            val ch = TunnelLink(server.url).open(ev)
            assertTrue(server.awaitConnected())
            server.send(0x2, byteArrayOf(0xA5.toByte(), 0x48, 1, 2, 3))
            assertArrayEquals(byteArrayOf(0xA5.toByte(), 0x48, 1, 2, 3), ev.binary.poll(5, TimeUnit.SECONDS))
            assertTrue(ch.send(byteArrayOf(9, 8, 7)))
            val f = server.readFrame()
            assertNotNull(f)
            assertEquals(0x2, f!!.opcode)
            assertArrayEquals(byteArrayOf(9, 8, 7), f.payload)
            ch.close(1000)
        }
    }

    @Test
    fun aTextMessageCancelsTheSocketWithoutACloseHandshake() {
        TestWsServer().use { server ->
            val ev = Events()
            TunnelLink(server.url).open(ev)
            assertTrue(server.awaitConnected())
            server.send(0x1, "hello".toByteArray())
            assertNotNull("failure reported", ev.failures.poll(5, TimeUnit.SECONDS))
            // cancel() drops the socket. A close() would have put a close frame (opcode 8) on the wire first.
            assertNull("no close frame", server.readFrame())
        }
    }

    @Test
    fun aPeerCloseIsEchoedWithTheSameCode() {
        TestWsServer().use { server ->
            val ev = Events()
            TunnelLink(server.url).open(ev)
            assertTrue(server.awaitConnected())
            server.send(0x8, byteArrayOf(0x03, 0xE9.toByte())) // 1001 going away
            assertEquals(1001, ev.closed.poll(5, TimeUnit.SECONDS))
            val f = server.readFrame()
            assertNotNull(f)
            assertEquals(0x8, f!!.opcode)
            assertEquals(1001, ((f.payload[0].toInt() and 0xFF) shl 8) or (f.payload[1].toInt() and 0xFF))
        }
    }

    @Test
    fun aPeerCloseWithoutACodeIsAnsweredWith1000() {
        TestWsServer().use { server ->
            val ev = Events()
            TunnelLink(server.url).open(ev)
            assertTrue(server.awaitConnected())
            server.send(0x8, ByteArray(0)) // reads as 1005, which must never be sent back
            assertNotNull(ev.closed.poll(5, TimeUnit.SECONDS))
            val f = server.readFrame()
            assertNotNull(f)
            assertEquals(0x8, f!!.opcode)
            assertEquals(1000, ((f.payload[0].toInt() and 0xFF) shl 8) or (f.payload[1].toInt() and 0xFF))
        }
    }

    @Test
    fun closeEchoCodesAreOnlyTheOnesAWebSocketMayCarry() {
        for (ok in listOf(1000, 1001, 1002, 1003, 1007, 1008, 1011, 3000, 4999)) assertEquals(ok, TunnelLink.closeEchoCode(ok))
        for (bad in listOf(0, 999, 1004, 1005, 1006, 1015, 2000, 5000)) assertEquals("code $bad", 1000, TunnelLink.closeEchoCode(bad))
    }

    @Test
    fun theClientPingsEvery25SecondsSoADeadTunnelFails() {
        assertEquals(25_000, TunnelLink.defaultClient().pingIntervalMillis)
        assertEquals(Wire.WS_PING_MS, TunnelLink.defaultClient().pingIntervalMillis.toLong())
    }
}
