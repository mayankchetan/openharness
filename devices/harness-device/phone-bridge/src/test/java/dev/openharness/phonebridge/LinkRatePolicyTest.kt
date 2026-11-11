package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Virtual time: nowMs() is the clock, schedule() queues on it, advance() runs what is due in order. */
private class LinkTime : Clock, Scheduler {
    private class Item(val at: Long, val task: Runnable) {
        var cancelled = false
    }

    private val items = mutableListOf<Item>()
    private var now = 0L

    override fun nowMs(): Long = now
    override fun sleepMs(ms: Long) = advance(ms)

    override fun schedule(delayMs: Long, task: Runnable): Cancelable {
        val item = Item(now + delayMs, task)
        items.add(item)
        return Cancelable { item.cancelled = true }
    }

    /** Timers that are still due to run. A stopped policy must leave none. */
    fun live(): Int = items.count { !it.cancelled }

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

class LinkRatePolicyTest {
    private val time = LinkTime()
    private val sent = mutableListOf<Pair<Long, LinkRate>>()
    private val policy = LinkRatePolicy(time, time, { rate -> sent.add(time.nowMs() to rate) })

    @Test
    fun startsAtHighAndRequestsNothingBeforeTheChannelIsUp() {
        time.advance(60_000)
        assertTrue("no channel, no request (DialLink asked for HIGH on connect)", sent.isEmpty())
        assertEquals(0, time.live())
    }

    @Test
    fun fiveSecondsWithoutBytesAfterTheChannelOpensGoesBalanced() {
        policy.channelUp()
        time.advance(4_999)
        assertTrue(sent.isEmpty())
        time.advance(1)
        assertEquals(listOf(5_000L to LinkRate.BALANCED), sent)
    }

    @Test
    fun trafficPushesTheIdleTimeoutBackAndOnlyOneTimerIsLive() {
        policy.channelUp()
        time.advance(4_000)
        policy.onTraffic()
        assertEquals("one idle timer, not one per byte", 1, time.live())
        time.advance(4_999) // 8999: idle for 4999 ms, not yet
        assertTrue(sent.isEmpty())
        time.advance(1) // 9000: idle for 5000 ms
        assertEquals(listOf(9_000L to LinkRate.BALANCED), sent)
    }

    @Test
    fun trafficAfterBalancedGoesHighAtOnce() {
        policy.channelUp()
        time.advance(5_000)
        time.advance(300)
        policy.onTraffic()
        assertEquals(listOf(5_000L to LinkRate.BALANCED, 5_300L to LinkRate.HIGH), sent)
        time.advance(60_000) // the idle clock runs again after the traffic
        assertEquals(3, sent.size)
        assertEquals(LinkRate.BALANCED, sent[2].second)
    }

    @Test
    fun steadyTrafficWhileHighSendsNoRequest() {
        policy.channelUp()
        repeat(100) {
            policy.onTraffic()
            time.advance(40) // 4 s of traffic, inside the idle window
        }
        assertTrue("no flapping while bytes flow", sent.isEmpty())
    }

    @Test
    fun balancedStaysBalancedUntilTheNextByte() {
        policy.channelUp()
        time.advance(5_000)
        time.advance(60_000)
        assertEquals("one request, no repeats", 1, sent.size)
        assertEquals(0, time.live())
    }

    @Test
    fun aDowngradeWaitsTwoSecondsAfterTheLastRequest() {
        policy.channelUp()
        time.advance(5_000) // BALANCED at 5000
        time.advance(500)
        policy.onTraffic() // HIGH at 5500, at once
        time.advance(100)
        policy.channelDown() // 5600: BALANCED is due 2 s after the last request, at 7500
        time.advance(1_899)
        assertEquals(2, sent.size)
        time.advance(1)
        assertEquals(
            listOf(5_000L to LinkRate.BALANCED, 5_500L to LinkRate.HIGH, 7_500L to LinkRate.BALANCED),
            sent,
        )
    }

    @Test
    fun setupRaisesToHighAtOnceAndCancelsAPendingDowngrade() {
        policy.channelUp()
        time.advance(5_000) // BALANCED at 5000
        time.advance(500)
        policy.onTraffic() // HIGH at 5500
        time.advance(100)
        policy.channelDown() // BALANCED deferred to 7500
        time.advance(400)
        policy.setupStarted() // 6000: already HIGH, so nothing is sent, and the deferred request is dropped
        time.advance(30_000)
        assertEquals(listOf(5_000L to LinkRate.BALANCED, 5_500L to LinkRate.HIGH), sent)
        assertEquals("no timer left", 0, time.live())
    }

    @Test
    fun setupAfterBalancedGoesHighAtOnce() {
        policy.channelUp()
        time.advance(5_000) // BALANCED at 5000
        time.advance(100)
        policy.setupStarted() // a tunnel retry on the kept link
        assertEquals(listOf(5_000L to LinkRate.BALANCED, 5_100L to LinkRate.HIGH), sent)
    }

    @Test
    fun channelDownOnAnIdleLinkGoesBalancedAndStopsTheIdleClock() {
        policy.channelUp()
        time.advance(100)
        policy.onTraffic()
        time.advance(900)
        policy.channelDown() // 1000: no earlier request, so no wait
        assertEquals(listOf(1_000L to LinkRate.BALANCED), sent)
        time.advance(60_000)
        assertEquals(1, sent.size)
        assertEquals(0, time.live())
    }

    @Test
    fun stopCancelsEveryTimerAndLaterCallsRequestNothing() {
        policy.channelUp()
        time.advance(1_000)
        policy.onTraffic()
        policy.stop()
        assertEquals("stop leaves no timer", 0, time.live())
        time.advance(60_000)
        policy.onTraffic()
        policy.channelUp()
        policy.setupStarted()
        policy.channelDown()
        time.advance(60_000)
        assertTrue(sent.isEmpty())
        assertEquals(0, time.live())
    }

    @Test
    fun stopCancelsADeferredDowngrade() {
        policy.channelUp()
        time.advance(5_000) // BALANCED at 5000
        time.advance(500)
        policy.onTraffic() // HIGH at 5500
        time.advance(100)
        policy.channelDown() // BALANCED deferred
        time.advance(100)
        policy.stop()
        assertEquals(0, time.live())
        time.advance(10_000)
        assertEquals(2, sent.size)
    }
}

class ActivityDialChannelTest {
    private class ScriptedChannel(private val reads: MutableList<Int>) : DialChannel {
        val writes = mutableListOf<Int>()
        var closed = false
        override fun read(buf: ByteArray): Int = reads.removeAt(0)
        override fun write(buf: ByteArray, off: Int, len: Int) {
            writes.add(len)
        }
        override fun close() {
            closed = true
        }
    }

    @Test
    fun everyByteReadCountsAsActivityAndEndOfStreamDoesNot() {
        val inner = ScriptedChannel(mutableListOf(5, 0, 3, -1))
        var activity = 0
        val channel = ActivityDialChannel(inner) { activity++ }
        val buf = ByteArray(16)
        assertEquals(5, channel.read(buf))
        assertEquals(0, channel.read(buf))
        assertEquals(3, channel.read(buf))
        assertEquals(-1, channel.read(buf))
        assertEquals(2, activity)
    }

    @Test
    fun everyNonEmptyWriteCountsAndCloseIsPassedOn() {
        val inner = ScriptedChannel(mutableListOf())
        var activity = 0
        val channel = ActivityDialChannel(inner) { activity++ }
        channel.write(ByteArray(8), 0, 8)
        channel.write(ByteArray(8), 0, 0)
        channel.write(ByteArray(8), 2, 4)
        assertEquals(listOf(8, 0, 4), inner.writes)
        assertEquals(2, activity)
        channel.close()
        assertTrue(inner.closed)
    }
}
