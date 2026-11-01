// The USB probe of a wireless dial: the driver half (task W15). The decisions (hello, welcome, frame cap, cadence) are usb_probe_core.c's.
//
// Everything here is called from ONE task, the mode switch task (mode_glue.c). The USB driver is installed only while the mode switch wants the
// probe (a host is driving the port and the dial is on Wi-Fi) and uninstalled when it does not: with no host it costs nothing, and in USB mode it is
// never installed (cable_link owns the port then). Small rings: the driver needs an RX ring over 64 bytes and a TX ring over 0, and the probe's
// frames are tiny.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Install the driver, allocate the decoder buffer (PSRAM when it can, else internal) and start. False when either would not come up; nothing is
// left installed then, and the caller tries again later. Calling it while running is a no-op that returns true.
bool usb_probe_start(uint64_t now_ms);
// Uninstall the driver and free the buffer. Safe when not running.
void usb_probe_stop(void);
bool usb_probe_running(void);
// Read what the host sent (waiting up to `wait_ms` for the first byte) and say hello when one is due. Returns after at most about wait_ms + 20 ms.
void usb_probe_poll(uint64_t now_ms, uint32_t wait_ms);
// The newest welcome seen since the probe started: false until there is one.
bool usb_probe_last_welcome(uint64_t *at_ms);
