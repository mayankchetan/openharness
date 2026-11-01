package dev.openharness.phonebridge

import okhttp3.Interceptor
import okhttp3.OkHttpClient
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.util.concurrent.atomic.AtomicReference

/** What the daemon's relay endpoint demands of the phone (track B, B3b): binary only, at most 64 KiB a message, no Origin. */
class TunnelContractTest {

    private val limit = 64 * 1024

    @Test
    fun pumpNeverSendsAMessageAbove64KiB() {
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val pump = Pump(dial, tunnel, RecordingListener())
        pump.start()
        val data = randomPayload(61, 1_000_000)
        chunks(data, 62, 300_000).forEach { dial.feed(it) } // dial chunks far above the limit
        waitUntil(what = "all bytes sent") { tunnel.sentBytes().size >= data.size }
        assertTrue("largest message ${tunnel.maxSend.get()} must be <= $limit", tunnel.maxSend.get() <= limit)
        assertTrue(Pump.CHUNK_BYTES <= limit)
        assertTrue("sanity: messages were sent", tunnel.sendCalls.get() > 0)
        pump.stop()
    }

    @Test
    fun tunnelLinkSetsNoOriginHeader() {
        val seen = AtomicReference<okhttp3.Request?>(null)
        val client = OkHttpClient.Builder()
            .addInterceptor(Interceptor { chain ->
                seen.set(chain.request())
                throw IOException("stop before any network use")
            })
            .build()
        val failed = AtomicReference<Int?>(-1)
        val link = TunnelLink("ws://127.0.0.1:18481/relay", mapOf("X-Test" to "1"), client)
        link.open(object : TunnelEvents {
            override fun onOpen() {}
            override fun onBinary(bytes: ByteArray) {}
            override fun onClosed(code: Int) {}
            override fun onFailure(httpCode: Int?, cause: Throwable) { failed.set(httpCode) }
        })
        waitUntil(what = "interceptor saw the request") { seen.get() != null }
        val req = seen.get()!!
        assertNull("no Origin header", req.header("Origin"))
        assertEquals("custom headers still pass", "1", req.header("X-Test"))
        waitUntil(what = "failure callback") { failed.get() != -1 }
    }
}
