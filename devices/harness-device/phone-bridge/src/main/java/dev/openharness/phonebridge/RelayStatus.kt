package dev.openharness.phonebridge

/** Turns the app state into the relay-status byte of spec section 5.1. Pure: no Android classes. */
object RelayStatusMapper {
    fun map(s: BridgeSnapshot, hasNetwork: Boolean): RelayStatus {
        return when {
            s.phase == Phase.TOKEN_REJECTED -> RelayStatus.TOKEN_REJECTED
            s.phase == Phase.TUNNEL_UP -> RelayStatus.TUNNEL_UP
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

    /** No write is out and none waits. */
    @Synchronized
    fun isIdle(): Boolean = !inFlight && pending == null

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

    /** Starts writing with `write` and sends the current state at once. The result is the token for detach() and awaitIdle(). */
    @Synchronized
    fun attach(write: (RelayStatus) -> Boolean): Any {
        val q = StatusWriteQueue(write)
        queue = q
        q.submit(RelayStatusMapper.map(snapshot, hasNetwork()))
        return q
    }

    /** Stops writing. With a token, only if that attach is still the current one (a late detach of an old link does nothing). */
    @Synchronized
    fun detach(token: Any? = null) {
        if (token == null || token === queue) queue = null
        (this as Object).notifyAll()
    }

    /**
     * Waits until the write queue of `token` has no write out and none waiting, at most `timeoutMs`. True if idle.
     * A detached or replaced token counts as idle. Call it off the main thread.
     */
    @Synchronized
    fun awaitIdle(token: Any, timeoutMs: Long): Boolean {
        val end = System.nanoTime() + timeoutMs * 1_000_000
        while (queue === token && !(token as StatusWriteQueue).isIdle()) {
            val left = (end - System.nanoTime()) / 1_000_000
            if (left <= 0) return false
            (this as Object).wait(left)
        }
        return true
    }

    @Synchronized
    fun onState(s: BridgeSnapshot) {
        snapshot = s
        queue?.submit(RelayStatusMapper.map(s, hasNetwork()))
    }

    @Synchronized
    fun onWriteDone() {
        queue?.onWriteDone()
        (this as Object).notifyAll()
    }

    companion object {
        /** Set by BridgeApp at process start. Null in JVM tests. */
        @Volatile var shared: StatusPublisher? = null
    }
}
