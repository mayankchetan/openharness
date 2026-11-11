package dev.openharness.phonebridge

/** The connection priority the phone asks for. DialLink maps it to the BluetoothGatt constant. */
enum class LinkRate { HIGH, BALANCED }

/**
 * Picks the connection priority of the phone's link (spec section 3). HIGH while the channel is being set up and while
 * bytes move in either direction; BALANCED once the open channel has carried no byte for `idleMs`.
 *
 * - Traffic (either direction) asks for HIGH at once, even inside the gap, so a voice upload is never held back.
 * - A downgrade to BALANCED waits until `minGapMs` after the last request, so the rate cannot flap.
 * - The idle clock runs only while a channel is up. Setup (before the channel) stays HIGH.
 * - stop() cancels every timer, and nothing is requested after it.
 *
 * Not thread-safe: the caller runs it on the main thread (activity from other threads is posted there).
 */
class LinkRatePolicy(
    private val clock: Clock,
    private val scheduler: Scheduler,
    private val request: (LinkRate) -> Unit,
    private val idleMs: Long = IDLE_MS,
    private val minGapMs: Long = MIN_GAP_MS,
) {
    /** The rate last requested. DialLink requests HIGH when the GATT link comes up, so the policy starts there. */
    private var current = LinkRate.HIGH
    private var lastRequestMs: Long? = null
    private var channelUp = false
    private var lastActivityMs = 0L
    private var idleTimer: Cancelable? = null
    private var gapTimer: Cancelable? = null
    private var stopped = false

    /** The channel is being set up (first open, or a tunnel retry on the kept link): HIGH now. */
    fun setupStarted() {
        if (stopped) return
        channelUp = false
        stopIdle()
        want(LinkRate.HIGH)
    }

    /** The channel opened: the idle clock starts now. */
    fun channelUp() {
        if (stopped) return
        channelUp = true
        lastActivityMs = clock.nowMs()
        armIdle(idleMs)
    }

    /** The channel closed. Without a channel the link is idle, so BALANCED (subject to the gap). */
    fun channelDown() {
        if (stopped) return
        channelUp = false
        stopIdle()
        want(LinkRate.BALANCED)
    }

    /** Bytes moved on the open channel, in either direction. Late reports from a closed channel are ignored. */
    fun onTraffic() {
        if (stopped || !channelUp) return
        lastActivityMs = clock.nowMs()
        if (idleTimer == null) armIdle(idleMs) // the idle check re-arms itself from lastActivityMs
        want(LinkRate.HIGH)
    }

    /** Cancels every timer. Safe to call twice. */
    fun stop() {
        stopped = true
        channelUp = false
        stopIdle()
        cancelGap()
    }

    /** The rate last requested. */
    fun currentRate(): LinkRate = current

    private fun armIdle(delayMs: Long) {
        idleTimer?.cancel()
        idleTimer = scheduler.schedule(delayMs) { onIdleCheck() }
    }

    private fun stopIdle() {
        idleTimer?.cancel()
        idleTimer = null
    }

    private fun onIdleCheck() {
        idleTimer = null
        if (stopped || !channelUp) return
        val idleFor = clock.nowMs() - lastActivityMs
        if (idleFor >= idleMs) want(LinkRate.BALANCED) else armIdle(idleMs - idleFor)
    }

    private fun want(rate: LinkRate) {
        if (stopped) return
        if (rate == LinkRate.HIGH) {
            cancelGap()
            if (current != LinkRate.HIGH) send(LinkRate.HIGH)
            return
        }
        if (current == LinkRate.BALANCED) {
            cancelGap()
            return
        }
        if (gapTimer != null) return
        val last = lastRequestMs
        val wait = if (last == null) 0L else maxOf(0L, last + minGapMs - clock.nowMs())
        if (wait == 0L) {
            send(LinkRate.BALANCED)
        } else {
            gapTimer = scheduler.schedule(wait) {
                gapTimer = null
                if (!stopped && current != LinkRate.BALANCED) send(LinkRate.BALANCED)
            }
        }
    }

    private fun send(rate: LinkRate) {
        current = rate
        lastRequestMs = clock.nowMs()
        request(rate)
    }

    private fun cancelGap() {
        gapTimer?.cancel()
        gapTimer = null
    }

    companion object {
        /** No byte in either direction for this long, with the channel up, means the link is idle. */
        const val IDLE_MS = 5_000L

        /** Minimum time between two priority requests for a downgrade. Upgrades are not held back. */
        const val MIN_GAP_MS = 2_000L
    }
}

/**
 * Wraps the channel and reports every byte moved, in either direction, so the link policy sees a voice upload from the
 * dial on the first packet. Reports come on the pump threads: the caller posts them to the main thread.
 */
class ActivityDialChannel(private val inner: DialChannel, private val onActivity: () -> Unit) : DialChannel {
    override fun read(buf: ByteArray): Int {
        val n = inner.read(buf)
        if (n > 0) onActivity()
        return n
    }

    override fun write(buf: ByteArray, off: Int, len: Int) {
        inner.write(buf, off, len)
        if (len > 0) onActivity()
    }

    override fun close() = inner.close()
}
