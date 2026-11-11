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
    private var started = false
    private var sawBonding = false
    private var done = false

    /** The first look at the bond state, after the GATT services are known. */
    fun start(current: BondState): BondStep {
        started = true
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

    /**
     * The link dropped before the channel opened. While a pairing still waits for an answer, the drop ends it as a failed
     * pairing: a retry would start a new pairing prompt on both devices. Otherwise it is not a pairing matter.
     */
    fun onLinkLost(): BondStep = if (!started || done) BondStep.Wait else finish(BondStep.Fail(FAILED_TEXT))

    private fun finish(step: BondStep): BondStep {
        done = true
        return step
    }

    companion object {
        /** How long to wait for the user to answer on both devices. The dial keeps its pair window open for 120 s. */
        const val TIMEOUT_MS = 60_000L
        const val FAILED_TEXT =
            "Pairing failed - press Start to try again; if the dial was reset or forgotten, also remove it in Android Bluetooth settings"
    }
}

/** What kind of failure a link attempt reports. The caller picks the response: the retry ladder, or a stop with a message. */
enum class FailureKind {
    /** Scan, connect, service or channel failure that may get better: the retry ladder tries again. */
    LINK,

    /** The pairing step failed (refused, timed out, createBond refused). No retry: the user presses Start again. */
    PAIRING,

    /** The channel keeps failing while Android reads the dial as bonded: the dial has forgotten this phone. No retry. */
    STALE_BOND,
}

/**
 * Counts channel failures in a row while Android reads the dial as bonded. Two in a row mean the dial no longer holds the
 * bond this phone has (a Forget on the dial, `harness dial phone forget` or `harness dial wifi forget`, or the BOOT reset).
 * Android does not pair a device it holds as bonded, so a retry cannot fix it.
 *
 * It must outlive one DialLink: every retry of the ladder makes a new link, so the BridgeService owns the counter. Pure:
 * no Android types. A channel that opens clears the count, and so does a user Start (reset()).
 */
class StaleBondCounter(private val limit: Int = LIMIT) {
    private var streak = 0

    /**
     * A channel attempt failed. `bonded`: Android reads BOND_BONDED for the dial now. A failure without a bond is an
     * ordinary link failure and breaks the streak. Returns the kind to report for this failure.
     */
    @Synchronized
    fun onChannelFailed(bonded: Boolean): FailureKind {
        if (!bonded) {
            streak = 0
            return FailureKind.LINK
        }
        streak++
        return if (streak >= limit) FailureKind.STALE_BOND else FailureKind.LINK
    }

    /** A channel opened: the bond works. */
    @Synchronized
    fun onChannelOpened() {
        streak = 0
    }

    /** A new user Start: the count begins again. */
    @Synchronized
    fun reset() {
        streak = 0
    }

    companion object {
        const val LIMIT = 2
        const val STALE_TEXT =
            "The dial does not know this phone any more - remove it in Android Bluetooth settings, then press Start"
    }
}
