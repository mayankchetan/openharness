package dev.openharness.phonebridge

/** Bond state of the dial as the app sees it (BluetoothDevice.BOND_NONE, BOND_BONDING, BOND_BONDED). */
enum class BondState {
    NONE, BONDING, BONDED;

    companion object {
        fun fromAndroid(code: Int): BondState = when (code) {
            11 -> BONDING
            12 -> BONDED
            else -> NONE
        }
    }
}

/** What DialLink does next. */
sealed interface BondStep {
    /** The link is bonded (or the dev build skips bonding): open the L2CAP channel. */
    data object OpenChannel : BondStep

    /** Call createBond() on the dial. */
    data object StartBond : BondStep

    /** Do nothing: no teardown, no retry. */
    data object Wait : BondStep

    /** Pairing failed: report this text and let the caller tear the link down. */
    data class Fail(val detail: String) : BondStep
}

/**
 * Decides when the app may open the secure channel. Pure: no Android types, so it runs in JVM tests.
 *
 * The secure channel needs a bond. Without one, a failed connect ends the link, and the retry can cancel the pairing
 * prompt that Android shows. So the app bonds first and waits (BOND_BONDING never counts as a failure). A BOND_NONE
 * after BOND_BONDING is a refused pairing; a timer covers a pairing that never finishes.
 *
 * One instance per link. The caller feeds it on one thread (the main looper).
 */
class BondGate(private val allowUnbonded: Boolean) {
    private var sawBonding = false
    private var done = false

    /** The first look at the bond state, after the GATT services are known. */
    fun start(current: BondState): BondStep {
        if (allowUnbonded || current == BondState.BONDED) return finish(BondStep.OpenChannel)
        if (current == BondState.BONDING) sawBonding = true
        return if (current == BondState.BONDING) BondStep.Wait else BondStep.StartBond
    }

    /** A bond-state broadcast for the dial. */
    fun onBondChanged(state: BondState): BondStep {
        if (done) return BondStep.Wait
        return when (state) {
            BondState.BONDED -> finish(BondStep.OpenChannel)
            BondState.BONDING -> {
                sawBonding = true
                BondStep.Wait
            }
            // A BOND_NONE before any BOND_BONDING is the state we started from, not an answer.
            BondState.NONE -> if (sawBonding) finish(BondStep.Fail(FAILED_TEXT)) else BondStep.Wait
        }
    }

    /** createBond() returned false: Android would not start a pairing. */
    fun onCreateBondRefused(): BondStep = if (done) BondStep.Wait else finish(BondStep.Fail(FAILED_TEXT))

    /** The wait ran out. */
    fun onTimeout(): BondStep = if (done) BondStep.Wait else finish(BondStep.Fail(FAILED_TEXT))

    private fun finish(step: BondStep): BondStep {
        done = true
        return step
    }

    companion object {
        /** How long to wait for the user to answer on both devices. The dial keeps its pair window open for 120 s. */
        const val TIMEOUT_MS = 60_000L
        const val FAILED_TEXT = "Pairing refused or timed out - check the dial"
    }
}
