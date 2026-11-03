package dev.openharness.phonebridge

import java.util.Random

/**
 * The back-off ladder of spec rule 4: 1, 2, 4, 8, 16, 30 s, then 30 s for ever. A good connection calls reset().
 * With a `random`, each delay is cut by up to a quarter, so the delay never passes the step (and never the 30 s cap).
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
        return if (random == null) base else base - random.nextLong(base / 4 + 1)
    }

    @Synchronized
    fun reset() {
        next = 0
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

object ReconnectPolicy {
    /** The edge refuses a wrong or revoked service token with 401 or 403. */
    fun isTokenRejected(httpCode: Int?): Boolean = httpCode == 401 || httpCode == 403

    /** A user stop and a rejected token are not retried: the first is wanted, the second cannot get better. */
    fun shouldRetry(reason: CloseReason, httpCode: Int?): Boolean =
        reason != CloseReason.STOPPED && !isTokenRejected(httpCode)
}
