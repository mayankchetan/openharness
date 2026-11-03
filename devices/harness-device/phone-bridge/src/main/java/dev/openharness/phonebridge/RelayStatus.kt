package dev.openharness.phonebridge

/** Turns the app state into the relay-status byte of spec section 5.1. Pure: no Android classes. */
object RelayStatusMapper {
    /** The detail BridgeService sets when the WebSocket is up. A test checks that the service still uses it. */
    const val DETAIL_TUNNEL_UP = "tunnel up"

    fun map(s: BridgeSnapshot, hasNetwork: Boolean): RelayStatus {
        val tunnelUp = s.phase == Phase.RELAYING && s.detail == DETAIL_TUNNEL_UP
        return when {
            s.phase == Phase.TOKEN_REJECTED -> RelayStatus.TOKEN_REJECTED
            tunnelUp -> RelayStatus.TUNNEL_UP
            !hasNetwork && s.phase in NEEDS_NETWORK -> RelayStatus.PHONE_NO_NETWORK
            s.phase == Phase.RELAYING -> RelayStatus.TUNNEL_CONNECTING
            else -> RelayStatus.IDLE
        }
    }

    private val NEEDS_NETWORK = setOf(Phase.CHANNEL_OPEN, Phase.RELAYING, Phase.CLOSED, Phase.WAITING_RETRY)
}

/**
 * One GATT write at a time (Android refuses a second while one is out). While a write is out only the newest value waits,
 * because the dial wants the current state, not the history. A value equal to the one last sent is not sent again.
 */
class StatusWriteQueue(private val write: (RelayStatus) -> Boolean) {
    private var inFlight = false
    private var lastSent: RelayStatus? = null
    private var pending: RelayStatus? = null

    @Synchronized
    fun submit(s: RelayStatus) {
        if (inFlight) {
            pending = if (s == lastSent) null else s
        } else if (s != lastSent) {
            send(s)
        }
    }

    /** The write ended (the GATT callback), whatever the result. */
    @Synchronized
    fun onWriteDone() {
        inFlight = false
        val next = pending ?: return
        pending = null
        if (next != lastSent) send(next)
    }

    private fun send(s: RelayStatus) {
        inFlight = write(s)
        lastSent = if (inFlight) s else null // a write that did not start is tried again with the next change
    }
}

/**
 * Writes the relay-status characteristic on every state change (spec section 6.3 rule 8). The link attaches a writer once its
 * GATT services are known and detaches it when it closes; BridgeState changes arrive through onState. It holds no secret.
 */
class StatusPublisher(private val hasNetwork: () -> Boolean) {
    @Volatile private var snapshot = BridgeSnapshot()
    private var queue: StatusWriteQueue? = null

    /** Starts writing with `write` and sends the current state at once. */
    @Synchronized
    fun attach(write: (RelayStatus) -> Boolean) {
        val q = StatusWriteQueue(write)
        queue = q
        q.submit(RelayStatusMapper.map(snapshot, hasNetwork()))
    }

    @Synchronized
    fun detach() {
        queue = null
    }

    @Synchronized
    fun onState(s: BridgeSnapshot) {
        snapshot = s
        queue?.submit(RelayStatusMapper.map(s, hasNetwork()))
    }

    @Synchronized
    fun onWriteDone() {
        queue?.onWriteDone()
    }

    companion object {
        /** Set by BridgeApp at process start. Null in JVM tests. */
        @Volatile var shared: StatusPublisher? = null
    }
}
