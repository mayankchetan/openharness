// The USB probe of a wireless dial, the part that needs no driver (task W15; design MS section 2, daemon side in cableSession.ts, wire in
// docs/specs/cable-wifi-link.md section 5).
//
// A dial on Wi-Fi (or, later, Bluetooth) has no USB cable stack. While a host drives the USB port, though, the dial must still find out whether a
// DAEMON is there, because the mode switch (mode_switch.c) sends the dial back to USB only when one answers. The probe is a second, tiny USB
// reader: a driver with small rings, a hello with probe:1 every 2 s, and a decoder that looks for ONE thing in what comes back, a JSON frame whose
// top-level "t" is "welcome". Everything else is dropped. It is not a session: it attaches nothing, parses no roster, offers no firmware.
//
// This file is the pure half: the hello frame, the welcome test, the decoder with its frame cap, the cadence and the timestamp of the newest
// welcome. usb_probe.c installs the driver and moves bytes; the decisions are made here and tested on the host (test_usb_probe.c).
//
// Why the strictness matters. A probe that misses a welcome is a dial that never comes back from Wi-Fi to USB. A probe that takes something
// else for a welcome is a dial that reboots into USB when there is no daemon (a PC with the port held by another program). So the welcome test
// reads the top-level key "t" of one JSON object, exactly, and nothing it can be tricked by: not a nested "t", not a quoted one, not a log line
// that spells one, not a truncated or doubled one.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cable_frame.h"

// One frame, header and CRC included. A frame longer than this is noise to the probe's decoder (the header is stepped over and it resyncs on the
// next magic), exactly as for a bad CRC. MS suggested "about 1 KB, set from a daemon log": the daemon's probe welcome is under 100 bytes and a
// full welcome from an older daemon is a few hundred (cableSession.probe.spec.ts holds the first to 256).
#define UP_FRAME_CAP 1024u
// Hello cadence: HELLO_ALONE_MS of cable_client.c, and the figure the 3 s welcome-age limit of mode_switch.h was derived from.
#define UP_HELLO_MS 2000u

typedef struct {
    cable_decoder_buf_t dec;     // over the caller's UP_FRAME_CAP buffer
    bool     have_welcome;
    uint64_t last_welcome_ms;    // when the newest welcome arrived (the feed's clock)
    bool     hello_started;
    uint64_t next_hello_ms;
    uint64_t now_ms;             // the clock of the feed in progress, for the frame callback
} up_core_t;

// False (and `p` unusable: feeding it does nothing) for a NULL pointer or a buffer under UP_FRAME_CAP. The buffer must outlive the probe.
bool up_core_init(up_core_t *p, uint8_t *buf, size_t cap);
// A new plug-in (or the probe installed again): forget every welcome and any half-received frame; the first hello is due at once.
void up_core_start(up_core_t *p, uint64_t now_ms);
// Bytes from the port. `now_ms` stamps any welcome they complete.
void up_core_feed(up_core_t *p, const uint8_t *data, size_t n, uint64_t now_ms);
// True once per UP_HELLO_MS (and at once after up_core_start). A stall is one hello, not a burst. A clock that steps back does not fire.
bool up_core_hello_due(up_core_t *p, uint64_t now_ms);
// The hello as an encoded CABLE_TYPE_JSON frame: {"t":"hello","product":P,"proto":N,"probe":1}, and nothing else (no mac, no settings, no pet list).
// Returns the frame length, or 0 for a NULL argument, a product that needs JSON escaping, a negative proto or a buffer that is too small.
size_t up_core_hello(uint8_t *out, size_t cap, int proto, const char *product);

// Is this payload one JSON object whose top-level "t" is the string "welcome"? Exactly that, with nothing else (see the header comment).
bool up_is_welcome(const uint8_t *payload, size_t len);
