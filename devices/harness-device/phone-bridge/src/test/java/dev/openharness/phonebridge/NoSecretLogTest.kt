package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.PrintStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger

/** The service token must not reach any text sink: toString, stdout and stderr, java.util.logging, failure causes. */
class NoSecretLogTest {

    private val secret = "TOPSECRET-9f3a1c-value"
    private val clientId = "id-77aa.access"

    private fun settings(): TunnelSettings {
        val store = SettingsStore(FakeKv(), FakeCipher())
        assertEquals(null, store.save(SettingsStore.DEFAULT_RELAY_URL, clientId, secret))
        return store.load()
    }

    @Test
    fun toStringOfTheSecretAndTheSettingsIsRedacted() {
        val s = settings()
        assertFalse(s.toString(), s.toString().contains(secret))
        assertFalse(s.secret.toString().contains(secret))
        assertFalse("$s".contains(secret))
        assertEquals(secret, s.secret.reveal()) // the value is still there for the header
    }

    @Test
    fun storeAndLinkObjectsDoNotPrintTheSecret() {
        val s = settings()
        val link = TunnelLink.fromSettings(s)
        assertFalse(link.toString().contains(secret))
        assertFalse(SettingsStore(FakeKv(), FakeCipher()).toString().contains(secret))
    }

    @Test
    fun saveLoadAndAFailedConnectWriteNothingWithTheSecret() {
        val out = ByteArrayOutputStream()
        val err = ByteArrayOutputStream()
        val oldOut = System.out
        val oldErr = System.err
        val logged = StringBuilder()
        val root = Logger.getLogger("")
        val handler = object : Handler() {
            override fun publish(r: LogRecord) {
                logged.append(r.message).append(r.thrown?.toString())
            }
            override fun flush() {}
            override fun close() {}
        }
        val failure = LinkedBlockingQueue<Throwable>()
        System.setOut(PrintStream(out, true))
        System.setErr(PrintStream(err, true))
        root.addHandler(handler)
        try {
            val s = settings()
            s.toString()
            // Nothing listens on this port, so the connect fails; the cause must not carry the headers.
            val dead = java.net.ServerSocket(0).use { it.localPort }
            val link = TunnelLink("ws://127.0.0.1:$dead/relay", s.headers())
            link.open(object : TunnelEvents {
                override fun onOpen() {}
                override fun onBinary(bytes: ByteArray) {}
                override fun onClosed(code: Int) {}
                override fun onFailure(httpCode: Int?, cause: Throwable) {
                    failure.put(cause)
                }
            })
            val cause = failure.poll(10, TimeUnit.SECONDS)
            assertNotNull("a connect failure arrives", cause)
            assertFalse("cause: $cause", cause!!.toString().contains(secret))
            assertFalse(cause.stackTraceToString().contains(secret))
        } finally {
            root.removeHandler(handler)
            System.setOut(oldOut)
            System.setErr(oldErr)
        }
        val sinks = listOf("stdout" to out.toString(), "stderr" to err.toString(), "logging" to logged.toString())
        for ((name, text) in sinks) {
            assertFalse("$name holds the secret", text.contains(secret))
        }
    }

    @Test
    fun aHeaderValueOkHttpRefusesDoesNotLeakInTheException() {
        val link = TunnelLink("ws://127.0.0.1:1/relay", mapOf("CF-Access-Client-Secret" to "bad\nSECRETX"))
        val e = try {
            link.open(object : TunnelEvents {
                override fun onOpen() {}
                override fun onBinary(bytes: ByteArray) {}
                override fun onClosed(code: Int) {}
                override fun onFailure(httpCode: Int?, cause: Throwable) {}
            })
            null
        } catch (t: Exception) {
            t
        }
        assertNotNull("a bad header value is refused", e)
        assertFalse(e.toString(), e.toString().contains("SECRETX"))
        assertFalse(e!!.stackTraceToString().contains("SECRETX"))
    }
}
