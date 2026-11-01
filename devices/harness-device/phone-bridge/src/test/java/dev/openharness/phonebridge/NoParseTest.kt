package dev.openharness.phonebridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** The phone is a pipe (wire spec section 6, rule 1): it never looks inside the bytes it moves. */
class NoParseTest {

    private fun bytes(vararg v: Int) = ByteArray(v.size) { v[it].toByte() }

    @Test
    fun bytesThatLookLikeCableFramesPassUnchangedBothWays() {
        val samples = listOf(
            bytes(0xA5, 0x48),
            bytes(0xA5, 0x48, 0x01, 0x07, 0xFF, 0xFF),                       // header claims 65535 bytes, then nothing
            bytes(0xA5, 0x48, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00),           // empty payload, bad check value
            bytes(0xA5, 0x48, 0xA5, 0x48, 0xA5, 0x48),
            bytes(0x00, 0xA5, 0x48, 0x99, 0xA5),                             // magic split across the end
            randomPayload(51, 4096),
        )
        val dial = FakeDial()
        val tunnel = FakeTunnel()
        val pump = Pump(dial, tunnel, RecordingListener())
        pump.start()
        samples.forEach { dial.feed(it) }
        val total = samples.sumOf { it.size }
        waitUntil(what = "tunnel opened") { tunnel.openCount.get() == 1 }
        samples.forEach { tunnel.events!!.onBinary(it) }
        waitUntil(what = "up") { tunnel.sentBytes().size >= total }
        waitUntil(what = "down") { dial.writtenBytes().size >= total }
        val expected = samples.fold(ByteArray(0)) { a, b -> a + b }
        assertArrayEquals(expected, tunnel.sentBytes())
        assertArrayEquals(expected, dial.writtenBytes())
        assertEquals("the pump must not drop or split the stream into frames", total, tunnel.sentBytes().size)
        pump.stop()
    }

    @Test
    fun mainSourcesNeverMentionTheCableFrameFormat() {
        val dir = File("src/main/java")
        assertTrue("run from the project dir; ${dir.absolutePath} must exist", dir.isDirectory)
        val forbidden = listOf(
            Regex("0[xX][aA]5\\b"),                    // frame magic byte
            Regex("(?i)\\bA5\\s*48\\b"),
            Regex("(?i)crc"),
            Regex("(?i)json"),
            Regex("(?i)CABLE_PROTO"),
            Regex("(?i)(parse|decode|deframe)\\w*Frame"),
        )
        val files = dir.walkTopDown().filter { it.isFile && it.extension == "kt" }.toList()
        assertTrue("sources found", files.size >= 5)
        for (f in files) {
            val text = f.readText()
            for (rx in forbidden) {
                val m = rx.find(text)
                assertTrue("${f.name} matches forbidden pattern ${rx.pattern}: '${m?.value}'", m == null)
            }
        }
    }
}
