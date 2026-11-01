package dev.openharness.phonebridge

/** Time source. Tests use a fake whose sleep advances time at once, so a 4 s stall costs no real time. */
interface Clock {
    fun nowMs(): Long
    fun sleepMs(ms: Long)
}

object RealClock : Clock {
    override fun nowMs(): Long = System.nanoTime() / 1_000_000
    override fun sleepMs(ms: Long) {
        try {
            Thread.sleep(ms)
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        }
    }
}
