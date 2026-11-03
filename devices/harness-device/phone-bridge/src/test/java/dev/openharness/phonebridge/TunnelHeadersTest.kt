package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** B10c: the Access service token goes out as two headers on the upgrade and nowhere else. */
class TunnelHeadersTest {
    private class Quiet : TunnelEvents {
        override fun onOpen() {}
        override fun onBinary(bytes: ByteArray) {}
        override fun onClosed(code: Int) {}
        override fun onFailure(httpCode: Int?, cause: Throwable) {}
    }

    @Test
    fun theUpgradeCarriesTheTwoAccessHeadersAndTheUrlStaysClean() {
        val store = SettingsStore(FakeKv(), FakeCipher())
        store.save("wss://dial.example.org/relay", "the-client-id", "the-client-secret")
        TestWsServer().use { server ->
            val ch = TunnelLink.fromSettings(store.load(), urlOverride = server.url).open(Quiet())
            assertTrue(server.awaitConnected())
            assertEquals("the-client-id", server.requestHeaders["cf-access-client-id"])
            assertEquals("the-client-secret", server.requestHeaders["cf-access-client-secret"])
            assertEquals("GET /relay HTTP/1.1", server.requestLine)
            assertFalse(server.requestLine.contains("the-client"))
            ch.close(1000)
        }
    }

    @Test
    fun withoutATokenNoAccessHeaderIsSent() {
        val store = SettingsStore(FakeKv(), FakeCipher())
        TestWsServer().use { server ->
            val ch = TunnelLink.fromSettings(store.load(), urlOverride = server.url).open(Quiet())
            assertTrue(server.awaitConnected())
            assertFalse(server.requestHeaders.keys.any { it.startsWith("cf-access") })
            ch.close(1000)
        }
    }
}
