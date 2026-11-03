package dev.openharness.phonebridge

/** Texts of the pairing screen. Pairing starts on the dial; this phone only shows what it sees. */
object PairingHint {
    const val TEXT =
        "Pairing starts on the dial: Settings > Phone > Pair. The dial shows a 6-digit number and Android shows " +
            "one too. Accept only if the two numbers match."

    fun stateLine(phase: Phase): String = when (phase) {
        Phase.IDLE -> "The bridge is stopped. Start it from the main screen."
        Phase.SCANNING -> "Looking for the dial."
        Phase.CONNECTING -> "Found the dial. Connecting."
        Phase.CHANNEL_OPEN, Phase.RELAYING -> "Connected to the dial."
        Phase.CLOSED -> "The dial closed the link."
        Phase.FAILED -> "Could not reach the dial."
        Phase.NO_PERMISSION -> "Allow Nearby devices for this app."
        Phase.BLUETOOTH_OFF -> "Turn Bluetooth on."
        Phase.WAITING_RETRY -> "Waiting to look for the dial again."
        Phase.TOKEN_REJECTED -> "The tunnel refused the service token. Check Settings."
    }
}
