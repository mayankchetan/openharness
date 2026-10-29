// The physical keys (boot_key.h): BOOT (GPIO0) held = record, released = send; a tap interrupts the running turn. The PWR key toggles
// the screen. (GPIO0 doubles as the settings wipe, but ONLY when held for 5 s after power-on, with a countdown on the glass.)
#pragma once

// Start the button poll task. Call after ui_set_voice().
void ptt_start(void);
