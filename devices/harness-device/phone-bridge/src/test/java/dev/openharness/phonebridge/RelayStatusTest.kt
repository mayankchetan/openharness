package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Spec section 5.1 and 6.3 rule 8: the relay-status byte follows the app state; one write at a time; the latest value wins. */
class RelayStatusTest {

    private fun snap(phase: Phase, detail: String = "") = BridgeSnapshot(phase, detail)

    @Test
    fun mapsEachPhaseToTheStatusTheDialShows() {
        val up = true
        assertEquals(RelayStatus.IDLE, RelayStatusMapper.map(snap(Phase.CONNECTING), up))
        assertEquals(RelayStatus.IDLE, RelayStatusMapper.map(snap(Phase.CHANNEL_OPEN, "channel open, waiting for the dial"), up))
        assertEquals(RelayStatus.TUNNEL_CONNECTING, RelayStatusMapper.map(snap(Phase.RELAYING, "tunnel opening"), up))
        assertEquals(RelayStatus.TUNNEL_UP, RelayStatusMapper.map(snap(Phase.TUNNEL_UP, "tunnel up"), up))
        assertEquals(RelayStatus.TOKEN_REJECTED, RelayStatusMapper.map(snap(Phase.TOKEN_REJECTED), up))
        for (p in listOf(Phase.IDLE, Phase.SCANNING, Phase.CLOSED, Phase.FAILED, Phase.NO_PERMISSION, Phase.BLUETOOTH_OFF, Phase.WAITING_RETRY)) {
            assertEquals(p.name, RelayStatus.IDLE, RelayStatusMapper.map(snap(p), up))
        }
    }

    @Test
    fun noInternetShowsAsPhoneNoNetworkUnlessTheTunnelIsUp() {
        val down = false
        for (p in listOf(Phase.CHANNEL_OPEN, Phase.CLOSED, Phase.WAITING_RETRY)) {
            assertEquals(p.name, RelayStatus.PHONE_NO_NETWORK, RelayStatusMapper.map(snap(p), down))
        }
        assertEquals(RelayStatus.PHONE_NO_NETWORK, RelayStatusMapper.map(snap(Phase.RELAYING, "tunnel opening"), down))
        assertEquals(RelayStatus.TUNNEL_UP, RelayStatusMapper.map(snap(Phase.TUNNEL_UP, "tunnel up"), down))
        assertEquals(RelayStatus.TOKEN_REJECTED, RelayStatusMapper.map(snap(Phase.TOKEN_REJECTED), down))
        assertEquals(RelayStatus.IDLE, RelayStatusMapper.map(snap(Phase.SCANNING), down))
    }

    @Test
    fun theServiceSetsThePhaseTheMapperReadsForTunnelUp() {
        // BridgeService sets this phase; the mapper no longer depends on a detail text.
        val service = File("src/main/java/dev/openharness/phonebridge/BridgeService.kt").readText()
        assertTrue(service.contains("update(mySession, Phase.TUNNEL_UP,"))
    }

    @Test
    fun theTunnelUpStatusDoesNotDependOnTheDetailText() {
        assertEquals(RelayStatus.TUNNEL_UP, RelayStatusMapper.map(snap(Phase.TUNNEL_UP), true))
        assertEquals(RelayStatus.TUNNEL_UP, RelayStatusMapper.map(snap(Phase.TUNNEL_UP, "anything"), true))
        assertEquals(RelayStatus.TUNNEL_CONNECTING, RelayStatusMapper.map(snap(Phase.RELAYING, "tunnel up"), true))
    }

    @Test
    fun theQueueIsIdleWhenNoWriteIsOutAndNoneWaits() {
        val q = StatusWriteQueue { true }
        assertTrue(q.isIdle())
        q.submit(RelayStatus.TUNNEL_UP)
        assertFalse(q.isIdle())
        q.submit(RelayStatus.TOKEN_REJECTED)
        q.onWriteDone() // sends the waiting one
        assertFalse(q.isIdle())
        q.onWriteDone()
        assertTrue(q.isIdle())
    }

    @Test
    fun awaitIdleWaitsForTheLastWriteButNoLongerThanTheLimit() {
        val p = StatusPublisher { true }
        val token = p.attach { true }
        assertFalse("write is out", p.awaitIdle(token, 50))
        Thread { Thread.sleep(40); p.onWriteDone() }.start()
        val t0 = System.nanoTime()
        assertTrue(p.awaitIdle(token, 2000))
        assertTrue("returned at once after the callback", (System.nanoTime() - t0) / 1_000_000 < 1000)
    }

    @Test
    fun awaitIdleReturnsAtOnceForADetachedOrUnknownToken() {
        val p = StatusPublisher { true }
        val token = p.attach { true }
        p.detach(token)
        assertTrue(p.awaitIdle(token, 2000))
    }

    @Test
    fun aLateDetachOfAnOldLinkDoesNotDropTheNewLinksWriter() {
        val sent = mutableListOf<RelayStatus>()
        val p = StatusPublisher { true }
        val old = p.attach { true }
        p.detach(old)
        p.attach { sent.add(it); true }
        p.detach(old) // the old link's drain ends after the new link attached
        p.onWriteDone()
        p.onState(snap(Phase.TOKEN_REJECTED))
        assertEquals(RelayStatus.TOKEN_REJECTED, sent.last())
    }

    @Test
    fun writesTheFirstStatusAtOnceAndSkipsAnUnchangedOne() {
        val sent = mutableListOf<RelayStatus>()
        val q = StatusWriteQueue { sent.add(it); true }
        q.submit(RelayStatus.IDLE)
        assertEquals(listOf(RelayStatus.IDLE), sent)
        q.onWriteDone()
        q.submit(RelayStatus.IDLE)
        assertEquals(1, sent.size)
    }

    @Test
    fun whileAWriteIsOutOnlyTheLatestWaitsAndGoesOutWhenItEnds() {
        val sent = mutableListOf<RelayStatus>()
        val q = StatusWriteQueue { sent.add(it); true }
        q.submit(RelayStatus.TUNNEL_CONNECTING)
        q.submit(RelayStatus.TUNNEL_UP)
        q.submit(RelayStatus.TOKEN_REJECTED)
        assertEquals(listOf(RelayStatus.TUNNEL_CONNECTING), sent)
        q.onWriteDone()
        assertEquals(listOf(RelayStatus.TUNNEL_CONNECTING, RelayStatus.TOKEN_REJECTED), sent)
        q.onWriteDone()
        assertEquals(2, sent.size)
    }

    @Test
    fun aWriteThatCannotStartDoesNotBlockTheNextOne() {
        val sent = mutableListOf<RelayStatus>()
        var accept = false
        val q = StatusWriteQueue { sent.add(it); accept }
        q.submit(RelayStatus.IDLE)
        accept = true
        q.submit(RelayStatus.TUNNEL_UP)
        assertEquals(listOf(RelayStatus.IDLE, RelayStatus.TUNNEL_UP), sent)
    }

    @Test
    fun theLinkWritesTheCurrentStateWhenItAttachesAndEveryChangeAfter() {
        val sent = mutableListOf<RelayStatus>()
        val p = StatusPublisher { true }
        p.onState(snap(Phase.SCANNING)) // nothing attached yet: nothing written
        assertTrue(sent.isEmpty())
        p.attach { sent.add(it); true }
        assertEquals(listOf(RelayStatus.IDLE), sent)
        p.onWriteDone()
        p.onState(snap(Phase.RELAYING, "tunnel opening"))
        p.onWriteDone()
        p.onState(snap(Phase.TUNNEL_UP, "tunnel up"))
        p.onWriteDone()
        p.onState(snap(Phase.TUNNEL_UP, "tunnel up")) // the 1 s ticker: no repeat
        assertEquals(listOf(RelayStatus.IDLE, RelayStatus.TUNNEL_CONNECTING, RelayStatus.TUNNEL_UP), sent)
    }

    @Test
    fun afterDetachNothingIsWrittenAndAReattachStartsFresh() {
        val sent = mutableListOf<RelayStatus>()
        val p = StatusPublisher { true }
        p.attach { sent.add(it); true }
        p.onWriteDone()
        p.detach()
        p.onState(snap(Phase.TOKEN_REJECTED))
        assertEquals(1, sent.size)
        p.attach { sent.add(it); true }
        assertEquals(RelayStatus.TOKEN_REJECTED, sent.last())
        assertFalse(sent.size < 2)
    }

    @Test
    fun theNetworkStateIsReadAtEachChange() {
        val sent = mutableListOf<RelayStatus>()
        var net = true
        val p = StatusPublisher { net }
        p.attach { sent.add(it); true }
        p.onWriteDone()
        net = false
        p.onState(snap(Phase.CHANNEL_OPEN, "channel open, waiting for the dial"))
        assertEquals(RelayStatus.PHONE_NO_NETWORK, sent.last())
    }
}
