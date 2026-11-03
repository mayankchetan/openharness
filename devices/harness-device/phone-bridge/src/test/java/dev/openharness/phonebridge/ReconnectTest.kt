package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random

/** Fake scheduler on virtual time: advance() runs what is due, cancel() removes a task. */
private class FakeScheduler : Scheduler {
    private class Item(val at: Long, val task: Runnable, var cancelled: Boolean = false)

    private val items = mutableListOf<Item>()
    var now = 0L
        private set
    val delays = mutableListOf<Long>()

    override fun schedule(delayMs: Long, task: Runnable): Cancelable {
        delays.add(delayMs)
        val item = Item(now + delayMs, task)
        items.add(item)
        return Cancelable { item.cancelled = true }
    }

    fun pending(): Int = items.count { !it.cancelled }

    fun advance(ms: Long) {
        val end = now + ms
        while (true) {
            val next = items.filter { !it.cancelled }.minByOrNull { it.at } ?: break
            if (next.at > end) break
            now = next.at
            items.remove(next)
            next.task.run()
        }
        now = end
    }
}

class ReconnectTest {

    @Test
    fun ladderIsOneTwoFourEightSixteenThirtySeconds() {
        val ladder = ReconnectLadder()
        assertEquals(listOf(1000L, 2000L, 4000L, 8000L, 16000L, 30000L), List(6) { ladder.nextDelayMs() })
    }

    @Test
    fun ladderStaysAtTheCapOf30Seconds() {
        val ladder = ReconnectLadder()
        repeat(6) { ladder.nextDelayMs() }
        repeat(50) { assertEquals(30_000L, ladder.nextDelayMs()) }
    }

    @Test
    fun ladderStartsOverAfterAReset() {
        val ladder = ReconnectLadder()
        repeat(4) { ladder.nextDelayMs() }
        ladder.reset()
        assertEquals(1000L, ladder.nextDelayMs())
        assertEquals(2000L, ladder.nextDelayMs())
    }

    @Test
    fun jitterOnlyShortensAndNeverPassesTheCap() {
        val ladder = ReconnectLadder(random = Random(5))
        val base = Wire.BACKOFF_LADDER_MS + List(20) { 30_000L }
        for (b in base) {
            val d = ladder.nextDelayMs()
            assertTrue("$d must be in [${b * 3 / 4}, $b]", d in (b * 3 / 4)..b)
        }
    }

    @Test
    fun reconnectorRunsTheActionAfterEachLadderStep() {
        val sched = FakeScheduler()
        var runs = 0
        val r = Reconnector(ReconnectLadder(), sched) { runs++ }
        r.failed()
        sched.advance(999)
        assertEquals(0, runs)
        sched.advance(1)
        assertEquals(1, runs)
        r.failed()
        sched.advance(2000)
        assertEquals(2, runs)
        assertEquals(listOf(1000L, 2000L), sched.delays)
    }

    @Test
    fun reconnectorKeepsClimbingWhileEveryAttemptFailsAndCapsAt30() {
        val sched = FakeScheduler()
        lateinit var r: Reconnector
        r = Reconnector(ReconnectLadder(), sched) { r.failed() } // every attempt fails at once
        r.failed()
        sched.advance(10 * 60_000L)
        assertEquals(listOf(1000L, 2000L, 4000L, 8000L, 16000L, 30000L, 30000L), sched.delays.take(7))
        assertTrue(sched.delays.drop(5).all { it == 30_000L })
    }

    @Test
    fun aGoodConnectionResetsTheLadder() {
        val sched = FakeScheduler()
        val r = Reconnector(ReconnectLadder(), sched) { }
        repeat(3) { r.failed(); sched.advance(60_000) }
        r.connected()
        r.failed()
        assertEquals(1000L, sched.delays.last())
    }

    @Test
    fun aSecondFailureWhileOneRetryWaitsDoesNotScheduleAnother() {
        val sched = FakeScheduler()
        var runs = 0
        val r = Reconnector(ReconnectLadder(), sched) { runs++ }
        r.failed()
        r.failed()
        r.failed()
        assertEquals(1, sched.pending())
        sched.advance(60_000)
        assertEquals(1, runs)
    }

    @Test
    fun stopCancelsAWaitingRetryAndLaterFailuresSchedulesNothing() {
        val sched = FakeScheduler()
        var runs = 0
        val r = Reconnector(ReconnectLadder(), sched) { runs++ }
        r.failed()
        r.stop()
        assertEquals(0, sched.pending())
        sched.advance(60_000)
        assertEquals(0, runs)
        assertFalse(r.failed())
        assertEquals(0, sched.pending())
    }

    @Test
    fun resumeAfterAStopStartsFromTheFirstStep() {
        val sched = FakeScheduler()
        val r = Reconnector(ReconnectLadder(), sched) { }
        repeat(3) { r.failed(); sched.advance(60_000) }
        r.stop()
        r.resume()
        assertTrue(r.failed())
        assertEquals(1000L, sched.delays.last())
    }

    @Test
    fun policyDoesNotRetryAUserStopOrARejectedToken() {
        assertFalse(ReconnectPolicy.shouldRetry(CloseReason.STOPPED, null))
        assertFalse(ReconnectPolicy.shouldRetry(CloseReason.TUNNEL_FAILED, 401))
        assertFalse(ReconnectPolicy.shouldRetry(CloseReason.TUNNEL_FAILED, 403))
        assertTrue(ReconnectPolicy.isTokenRejected(401))
        assertTrue(ReconnectPolicy.isTokenRejected(403))
        assertFalse(ReconnectPolicy.isTokenRejected(502))
    }

    @Test
    fun policyRetriesEveryOtherEnd() {
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.TUNNEL_FAILED, null))
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.TUNNEL_FAILED, 502))
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.TUNNEL_CLOSED, null))
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.DIAL_CLOSED, null))
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.STALL_TO_TUNNEL, null))
        assertTrue(ReconnectPolicy.shouldRetry(CloseReason.STALL_TO_DIAL, null))
    }
}
