package dev.openharness.phonebridge

import java.util.Random

/**
 * The back-off ladder of spec rule 4: 1, 2, 4, 8, 16, 30 s, then 30 s for ever. A good connection calls reset().
 * With a `random`, each delay is lengthened by up to a quarter of its step, then capped at 30 s. So a delay is never
 * shorter than its step (never below the 1 s floor) and never longer than 30 s.
 */
class ReconnectLadder(
    private val steps: List<Long> = Wire.BACKOFF_LADDER_MS,
    private val random: Random? = null,
) {
    private var next = 0

    @Synchronized
    fun nextDelayMs(): Long {
        val base = steps[minOf(next, steps.size - 1)]
        if (next < steps.size) next++
        return if (random == null) base else minOf(base + random.nextLong(base / 4 + 1), MAX_DELAY_MS)
    }

    @Synchronized
    fun reset() {
        next = 0
    }

    private companion object {
        const val MAX_DELAY_MS = 30_000L
    }
}

fun interface Cancelable {
    fun cancel()
}

/** Runs a task after a delay. The service uses a Handler; tests use a fake that runs on virtual time. */
fun interface Scheduler {
    fun schedule(delayMs: Long, task: Runnable): Cancelable
}

/**
 * One reconnect loop. failed() schedules `action` after the next ladder step; connected() resets the ladder;
 * stop() cancels a waiting retry and refuses new ones until resume(). One retry waits at a time.
 */
class Reconnector(
    private val ladder: ReconnectLadder,
    private val scheduler: Scheduler,
    private val action: () -> Unit,
) {
    private var pending: Cancelable? = null
    private var stopped = false

    /** True if a retry was scheduled; false if stopped or one is already waiting. */
    @Synchronized
    fun failed(): Boolean {
        if (stopped || pending != null) return false
        pending = scheduler.schedule(ladder.nextDelayMs()) { fire() }
        return true
    }

    private fun fire() {
        synchronized(this) {
            if (stopped) return
            pending = null
        }
        action()
    }

    @Synchronized
    fun connected() = ladder.reset()

    /** The user stopped the bridge: nothing is retried until resume(). */
    @Synchronized
    fun stop() {
        stopped = true
        pending?.cancel()
        pending = null
    }

    @Synchronized
    fun resume() {
        stopped = false
        pending?.cancel()
        pending = null
        ladder.reset()
    }
}

/** What to do when a relay session ends (see ReconnectPolicy.decide). */
enum class SessionEnd { TOKEN_REJECTED, STOP, RETRY_LINK, RETRY_TUNNEL }

object ReconnectPolicy {
    /**
     * The one decision for a closed session. `httpCode` is the code the pump reported with the close, never read
     * later from the pump, so a teardown on another thread cannot hide a 401 or 403.
     */
    fun decide(reason: CloseReason, httpCode: Int?): SessionEnd = when {
        isTokenRejected(httpCode) -> SessionEnd.TOKEN_REJECTED
        reason == CloseReason.STOPPED -> SessionEnd.STOP
        // The dial closed its side: a BLE matter. Anything else is the tunnel.
        reason == CloseReason.DIAL_CLOSED -> SessionEnd.RETRY_LINK
        else -> SessionEnd.RETRY_TUNNEL
    }

    /** The edge refuses a wrong or revoked service token with 401 or 403. */
    fun isTokenRejected(httpCode: Int?): Boolean = httpCode == 401 || httpCode == 403

    /** A user stop and a rejected token are not retried: the first is wanted, the second cannot get better. */
    fun shouldRetry(reason: CloseReason, httpCode: Int?): Boolean =
        reason != CloseReason.STOPPED && !isTokenRejected(httpCode)
}
