package dev.openharness.phonebridge

import java.nio.ByteBuffer
import java.util.UUID

/**
 * Constants and tiny helpers of the BLE wire spec (docs/specs/cable-ble-link.md). Every value here is replayed
 * against the shared vectors by VectorConformanceTest. Change a number in the generator script first.
 */
object Wire {
    val SERVICE_UUID: UUID = UUID.fromString("e70301f0-bbe6-4d70-801c-657c8e99501d")
    val PSM_UUID: UUID = UUID.fromString("e70301f1-bbe6-4d70-801c-657c8e99501d")
    val STATUS_UUID: UUID = UUID.fromString("e70301f2-bbe6-4d70-801c-657c8e99501d")
    val HINT_UUID: UUID = UUID.fromString("e70301f3-bbe6-4d70-801c-657c8e99501d")

    /** Fixed L2CAP PSM. The PSM characteristic holds the same value; the fixed value is the contract. */
    const val PSM = 0x0080

    const val NAME_PREFIX = "harness-dial-"

    const val RELAY_QUEUE_BYTES = 65536L
    const val RELAY_FULL_CLOSE_MS = 4000L
    const val WS_PING_MS = 25000L

    /** Daemon relay endpoint contract (B3b): one binary WebSocket message is at most 64 KiB. Not a vector value. */
    const val MAX_WS_MESSAGE_BYTES = 64 * 1024

    /** The vector `const` lines this app relies on. The test checks each against the vectors. */
    val CONSTANTS: Map<String, Long> = mapOf(
        "relay_queue_bytes" to RELAY_QUEUE_BYTES,
        "relay_full_close_ms" to RELAY_FULL_CLOSE_MS,
        "ws_ping_ms" to WS_PING_MS,
    )

    /** WebSocket back-off ladder (rule 4). Used by the reconnect logic of B10b. */
    val BACKOFF_LADDER_MS: List<Long> = listOf(1000, 2000, 4000, 8000, 16000, 30000)

    /** Bytes of a UUID as they go on the air: last byte first. */
    fun uuidLittleEndian(u: UUID): ByteArray {
        val be = ByteBuffer.allocate(16).putLong(u.mostSignificantBits).putLong(u.leastSignificantBits).array()
        return be.reversedArray()
    }

    fun psmFromLittleEndian(b: ByteArray): Int {
        require(b.size >= 2) { "PSM needs 2 bytes" }
        return (b[0].toInt() and 0xFF) or ((b[1].toInt() and 0xFF) shl 8)
    }

    /** `harness-dial-<mac2>`: the last two bytes of the dial's base MAC, four lower-case hex digits. */
    fun dialName(mac: String): String {
        val parts = mac.split(':')
        require(parts.size == 6) { "MAC needs six bytes" }
        return NAME_PREFIX + (parts[4] + parts[5]).lowercase()
    }

    fun isDialName(name: String?): Boolean = name != null && name.startsWith(NAME_PREFIX)

    /** Home-hint characteristic value: 0x01 = the phone sees the home network. */
    fun encodeHint(seesHome: Boolean): ByteArray = byteArrayOf(if (seesHome) 1 else 0)

    /** The dial accepts exactly one byte, 0 or 1. */
    fun isValidHint(b: ByteArray): Boolean = b.size == 1 && (b[0].toInt() and 0xFF) <= 1
}

/** Values of the relay-status characteristic (spec section 5.1). Any other value reads as UNKNOWN. */
enum class RelayStatus(val code: Byte, val vectorName: String) {
    IDLE(0, "idle"),
    TUNNEL_CONNECTING(1, "tunnel_connecting"),
    TUNNEL_UP(2, "tunnel_up"),
    TOKEN_REJECTED(3, "token_rejected"),
    PHONE_NO_NETWORK(4, "phone_no_network"),
    UNKNOWN(0xFF.toByte(), "unknown");

    companion object {
        fun fromByte(b: Byte): RelayStatus = values().firstOrNull { it != UNKNOWN && it.code == b } ?: UNKNOWN
    }
}

/** The two advertising structures the phone cares about (flags, complete local name, 128-bit service UUID list). */
class AdRecord(val flags: Int?, val completeName: String?, val serviceUuids: List<UUID>) {
    companion object {
        fun parse(data: ByteArray): AdRecord {
            var flags: Int? = null
            var name: String? = null
            val uuids = mutableListOf<UUID>()
            var i = 0
            while (i < data.size) {
                val len = data[i].toInt() and 0xFF
                if (len == 0 || i + 1 + len > data.size) break
                val type = data[i + 1].toInt() and 0xFF
                val body = data.copyOfRange(i + 2, i + 1 + len)
                when (type) {
                    0x01 -> if (body.isNotEmpty()) flags = body[0].toInt() and 0xFF
                    0x09 -> name = body.toString(Charsets.UTF_8)
                    0x07 -> {
                        var p = 0
                        while (p + 16 <= body.size) {
                            val be = body.copyOfRange(p, p + 16).reversedArray()
                            val bb = ByteBuffer.wrap(be)
                            uuids.add(UUID(bb.long, bb.long))
                            p += 16
                        }
                    }
                }
                i += 1 + len
            }
            return AdRecord(flags, name, uuids)
        }
    }
}
