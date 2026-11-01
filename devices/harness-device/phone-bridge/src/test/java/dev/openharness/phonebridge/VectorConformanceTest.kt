package dev.openharness.phonebridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Replays the shared vectors (docs/specs/cable-ble-link.md, produced by gen_ble_vectors.py) against the phone side. */
class VectorConformanceTest {

    private class Line(val kind: String, val f: List<String>)

    private val lines: List<Line> by lazy {
        val text = javaClass.classLoader!!.getResourceAsStream("ble_status.txt")!!.readBytes().toString(Charsets.UTF_8)
        text.lines().filter { it.isNotBlank() && !it.startsWith("#") }.map {
            val parts = it.split('\t')
            Line(parts[0], parts.drop(1))
        }
    }

    private fun of(kind: String) = lines.filter { it.kind == kind }

    private fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    @Test
    fun everyLineHasAKnownKindAndTheRightFieldCount() {
        // A hint line with an empty value ends in two tabs' worth of fields; split keeps empty strings.
        val arity = mapOf("uuid" to 3, "psm" to 3, "adv" to 4, "status" to 2, "hint" to 2, "const" to 3, "ladder" to 2)
        assertTrue(lines.size > 30)
        for (l in lines) assertEquals("fields of ${l.kind}: ${l.f}", arity[l.kind], l.f.size)
    }

    @Test
    fun uuidsMatchInTextAndLittleEndianBytes() {
        val byRole = of("uuid").associateBy { it.f[0] }
        val expect = mapOf(
            "service" to Wire.SERVICE_UUID, "psm" to Wire.PSM_UUID,
            "status" to Wire.STATUS_UUID, "hint" to Wire.HINT_UUID,
        )
        assertEquals(expect.keys, byRole.keys)
        for ((role, uuid) in expect) {
            val l = byRole.getValue(role)
            assertEquals(role, l.f[1], uuid.toString())
            assertArrayEquals(role, hex(l.f[2]), Wire.uuidLittleEndian(uuid))
        }
    }

    @Test
    fun psmMatches() {
        val l = of("psm").single()
        assertEquals(l.f[0].toInt(), Wire.PSM)
        assertEquals(l.f[1], "0x%04x".format(Wire.PSM))
        assertEquals(Wire.PSM, Wire.psmFromLittleEndian(hex(l.f[2])))
    }

    @Test
    fun advertisingNameAndScanResponseParseToWhatTheDialSends() {
        val advs = of("adv")
        assertTrue(advs.size >= 5)
        for (l in advs) {
            val mac = l.f[0]
            val name = l.f[1]
            val adv = AdRecord.parse(hex(l.f[2]))
            val scan = AdRecord.parse(hex(l.f[3]))
            assertEquals(name, adv.completeName)
            assertEquals("flags for $mac", 0x06, adv.flags)
            assertEquals(listOf(Wire.SERVICE_UUID), scan.serviceUuids)
            assertEquals(name, Wire.dialName(mac))
            assertTrue(Wire.isDialName(name))
            assertEquals(17, name.length)
        }
        assertFalse(Wire.isDialName("dial-bench6"))
        assertFalse(Wire.isDialName(null))
    }

    @Test
    fun statusValuesMapToTheSpecNames() {
        for (l in of("status")) {
            val v = hex(l.f[0]).single()
            assertEquals("status ${l.f[0]}", l.f[1], RelayStatus.fromByte(v).vectorName)
        }
        // The phone writes these codes; they must read back as themselves.
        for (s in RelayStatus.values().filter { it != RelayStatus.UNKNOWN }) {
            assertEquals(s, RelayStatus.fromByte(s.code))
        }
    }

    @Test
    fun homeHintEncodingMatchesTheOkVectorsAndRejectsTheErrOnes() {
        for (l in of("hint")) {
            val bytes = hex(l.f[0])
            if (l.f[1].startsWith("ok:")) {
                val n = l.f[1].removePrefix("ok:").toInt()
                assertTrue(Wire.isValidHint(bytes))
                assertArrayEquals(bytes, Wire.encodeHint(n == 1))
            } else {
                assertFalse("hint ${l.f[0]} must be invalid", Wire.isValidHint(bytes))
            }
        }
    }

    @Test
    fun constantsTheAppUsesMatch() {
        val vec = of("const").associate { it.f[0] to it.f[1].toLong() }
        assertTrue(Wire.CONSTANTS.isNotEmpty())
        for ((name, value) in Wire.CONSTANTS) {
            assertEquals("const $name", vec[name], value)
        }
        assertEquals(Wire.RELAY_QUEUE_BYTES, vec["relay_queue_bytes"])
        assertEquals(Wire.RELAY_FULL_CLOSE_MS, vec["relay_full_close_ms"])
        assertEquals(Wire.WS_PING_MS, vec["ws_ping_ms"])
    }

    @Test
    fun backoffLadderMatches() {
        val l = of("ladder").single()
        assertEquals(l.f[0].split(',').map { it.toLong() }, Wire.BACKOFF_LADDER_MS)
    }
}
