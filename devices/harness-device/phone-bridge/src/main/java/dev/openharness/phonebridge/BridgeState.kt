package dev.openharness.phonebridge

import java.util.concurrent.CopyOnWriteArrayList

enum class Phase {
    IDLE,
    SCANNING,
    CONNECTING,
    CHANNEL_OPEN,
    RELAYING,
    TUNNEL_UP,
    CLOSED,
    FAILED,
    NO_PERMISSION,
    BLUETOOTH_OFF,
    WAITING_RETRY,
    TOKEN_REJECTED,
}

/** What the status screen shows. `detail` is a fixed text; it never holds a URL, a header or a token. */
data class BridgeSnapshot(
    val phase: Phase = Phase.IDLE,
    val detail: String = "",
    val bytesToTunnel: Long = 0,
    val bytesToDial: Long = 0,
)

/** Process-wide status holder. The service writes it; the activity listens. Safe from any thread. */
object BridgeState {
    @Volatile
    var snapshot: BridgeSnapshot = BridgeSnapshot()
        private set

    private val listeners = CopyOnWriteArrayList<(BridgeSnapshot) -> Unit>()

    fun set(phase: Phase, detail: String = "", up: Long = 0, down: Long = 0) {
        val s = BridgeSnapshot(phase, detail, up, down)
        snapshot = s
        listeners.forEach { it(s) }
    }

    fun addListener(l: (BridgeSnapshot) -> Unit) {
        listeners.add(l)
    }

    fun removeListener(l: (BridgeSnapshot) -> Unit) {
        listeners.remove(l)
    }
}
