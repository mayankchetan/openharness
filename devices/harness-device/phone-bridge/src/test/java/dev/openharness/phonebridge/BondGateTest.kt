package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BondGateTest {
    private fun fail(step: BondStep): BondStep.Fail {
        assertTrue(step.toString(), step is BondStep.Fail)
        return step as BondStep.Fail
    }

    @Test
    fun alreadyBondedGoesStraightToTheChannel() {
        assertEquals(BondStep.OpenChannel, BondGate(allowUnbonded = false).start(BondState.BONDED))
    }

    @Test
    fun devBuildNeverBondsWhateverTheState() {
        for (s in BondState.values()) assertEquals(s.name, BondStep.OpenChannel, BondGate(allowUnbonded = true).start(s))
    }

    @Test
    fun unbondedStartsABondAndWaits() {
        val g = BondGate(false)
        assertEquals(BondStep.StartBond, g.start(BondState.NONE))
        assertEquals(BondStep.Wait, g.onBondChanged(BondState.BONDING))
        assertEquals(BondStep.OpenChannel, g.onBondChanged(BondState.BONDED))
    }

    @Test
    fun bondingThatAlreadyRunsIsNotStartedAgain() {
        val g = BondGate(false)
        assertEquals(BondStep.Wait, g.start(BondState.BONDING))
        assertEquals(BondStep.OpenChannel, g.onBondChanged(BondState.BONDED))
    }

    @Test
    fun bondingNeverFailsWhileItRuns() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        repeat(5) { assertEquals(BondStep.Wait, g.onBondChanged(BondState.BONDING)) }
    }

    @Test
    fun noneAfterBondingIsAFailedPairing() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        g.onBondChanged(BondState.BONDING)
        val f = fail(g.onBondChanged(BondState.NONE))
        assertEquals(BondGate.FAILED_TEXT, f.detail)
        assertTrue(f.detail, f.detail.startsWith("Pairing failed - press Start to try again"))
    }

    @Test
    fun pairingFailedTextTellsTheUserToRemoveAForgottenDial() {
        assertTrue(BondGate.FAILED_TEXT, BondGate.FAILED_TEXT.contains("if the dial was reset or forgotten"))
        assertTrue(BondGate.FAILED_TEXT, BondGate.FAILED_TEXT.contains("remove it in Android Bluetooth settings"))
    }

    @Test
    fun noneBeforeBondingStartsIsNotAFailure() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        assertEquals(BondStep.Wait, g.onBondChanged(BondState.NONE))
        assertEquals(BondStep.Wait, g.onBondChanged(BondState.BONDING))
        fail(g.onBondChanged(BondState.NONE))
    }

    @Test
    fun noneAfterAnAlreadyRunningBondingIsAFailure() {
        val g = BondGate(false)
        g.start(BondState.BONDING)
        fail(g.onBondChanged(BondState.NONE))
    }

    @Test
    fun timeoutFailsWhileWaiting() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        g.onBondChanged(BondState.BONDING)
        assertEquals(BondGate.FAILED_TEXT, fail(g.onTimeout()).detail)
    }

    @Test
    fun createBondRefusedFailsAtOnce() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        assertEquals(BondGate.FAILED_TEXT, fail(g.onCreateBondRefused()).detail)
    }

    @Test
    fun nothingHappensAfterTheOutcomeIsFinal() {
        val ok = BondGate(false)
        ok.start(BondState.NONE)
        ok.onBondChanged(BondState.BONDED)
        assertEquals(BondStep.Wait, ok.onTimeout())
        assertEquals(BondStep.Wait, ok.onBondChanged(BondState.NONE))

        val bad = BondGate(false)
        bad.start(BondState.BONDING)
        fail(bad.onBondChanged(BondState.NONE))
        assertEquals(BondStep.Wait, bad.onTimeout())
        assertEquals(BondStep.Wait, bad.onBondChanged(BondState.BONDED))
    }

    @Test
    fun timeoutIsAboutAMinute() {
        assertEquals(60_000L, BondGate.TIMEOUT_MS)
    }

    @Test
    fun bondStateMapsFromTheAndroidCodes() {
        assertEquals(BondState.NONE, BondState.fromAndroid(10))
        assertEquals(BondState.BONDING, BondState.fromAndroid(11))
        assertEquals(BondState.BONDED, BondState.fromAndroid(12))
        assertEquals(BondState.NONE, BondState.fromAndroid(-1))
    }

    // --- A link drop while the pairing still waits for an answer: a refused pairing, not a link problem. ---

    @Test
    fun aLinkLostWhilePairingIsAPairingFailure() {
        val g = BondGate(false)
        g.start(BondState.NONE)
        assertEquals(BondGate.FAILED_TEXT, fail(g.onLinkLost()).detail)
    }

    @Test
    fun aLinkLostWhileBondingRunsIsAPairingFailure() {
        val g = BondGate(false)
        g.start(BondState.BONDING)
        assertEquals(BondGate.FAILED_TEXT, fail(g.onLinkLost()).detail)
    }

    @Test
    fun aLinkLostBeforeAnyPairingStartedIsNotAPairingFailure() {
        assertEquals(BondStep.Wait, BondGate(false).onLinkLost())
    }

    @Test
    fun aLinkLostAfterTheOutcomeIsFinalIsNotAPairingFailure() {
        val bonded = BondGate(false)
        bonded.start(BondState.BONDED)
        assertEquals(BondStep.Wait, bonded.onLinkLost())

        val failed = BondGate(false)
        failed.start(BondState.NONE)
        failed.onBondChanged(BondState.BONDING)
        fail(failed.onBondChanged(BondState.NONE))
        assertEquals(BondStep.Wait, failed.onLinkLost())
    }

    // --- Stale bond: Android reads BONDED, but the channel keeps failing (the dial forgot this phone). ---

    @Test
    fun oneBondedChannelFailureIsAnOrdinaryLinkFailure() {
        assertEquals(FailureKind.LINK, StaleBondCounter().onChannelFailed(bonded = true))
    }

    @Test
    fun twoConsecutiveBondedChannelFailuresAreAStaleBond() {
        val c = StaleBondCounter()
        assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = true))
        assertEquals(FailureKind.STALE_BOND, c.onChannelFailed(bonded = true))
    }

    @Test
    fun staleBondStaysReportedUntilAReset() {
        val c = StaleBondCounter()
        c.onChannelFailed(bonded = true)
        c.onChannelFailed(bonded = true)
        assertEquals(FailureKind.STALE_BOND, c.onChannelFailed(bonded = true))
        c.reset()
        assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = true))
    }

    @Test
    fun aSuccessfulChannelStartsTheCountAgain() {
        val c = StaleBondCounter()
        c.onChannelFailed(bonded = true)
        c.onChannelOpened()
        assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = true))
        assertEquals(FailureKind.STALE_BOND, c.onChannelFailed(bonded = true))
    }

    @Test
    fun aFailureWithoutABondBreaksTheStreak() {
        val c = StaleBondCounter()
        c.onChannelFailed(bonded = true)
        assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = false))
        assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = true))
    }

    @Test
    fun unbondedFailuresAreNeverAStaleBond() {
        val c = StaleBondCounter()
        repeat(5) { assertEquals(FailureKind.LINK, c.onChannelFailed(bonded = false)) }
    }

    @Test
    fun staleBondLimitIsTwoAndTheTextSaysToRemoveTheDial() {
        assertEquals(2, StaleBondCounter.LIMIT)
        assertEquals(
            "The dial does not know this phone any more - remove it in Android Bluetooth settings, then press Start",
            StaleBondCounter.STALE_TEXT,
        )
    }
}
