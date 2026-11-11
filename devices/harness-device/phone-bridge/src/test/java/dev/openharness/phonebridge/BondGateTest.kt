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
        assertTrue(f.detail, f.detail.contains("Pairing refused or timed out"))
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
}
