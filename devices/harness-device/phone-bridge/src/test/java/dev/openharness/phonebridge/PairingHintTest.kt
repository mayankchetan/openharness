package dev.openharness.phonebridge

import org.junit.Assert.assertTrue
import org.junit.Test

class PairingHintTest {
    @Test
    fun hintSaysWhereToStartAndThatTheNumbersMustMatch() {
        val h = PairingHint.TEXT
        assertTrue(h, h.contains("Settings > Phone > Pair"))
        assertTrue(h, h.contains("6-digit"))
        assertTrue(h, h.contains("match"))
    }

    @Test
    fun everyPhaseHasAPairingLine() {
        Phase.values().forEach { assertTrue(it.name, PairingHint.stateLine(it).isNotEmpty()) }
    }
}
