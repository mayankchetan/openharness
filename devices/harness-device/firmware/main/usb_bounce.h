#pragma once
// The D+ bounce (battery sleep only; usb_bounce.c). After a light sleep the USB-Serial-JTAG block does not answer a host that was plugged in while
// the chip slept: D+ stayed pulled up, the host tried to enumerate once, failed, and does not try again (IDF usb-serial-jtag-console.rst, the sleep plan's
// risk R1, and the 2026-11-01 watchdog hang on dial :50, which came with a cable re-attached). Dropping the D+ pull-up for 100 ms looks to the host
// like an unplug, so it enumerates again when the pull-up comes back. Blocks the caller for that time: the render task, once per plug-in.
void usb_dp_bounce(void);
