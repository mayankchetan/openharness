#pragma once
// KEEPALIVE TIMING (task W13, docs/specs/cable-wifi-link.md "Keepalive"). Pure and IDF-free, so a host test holds the rule and the daemon's
// spec reads the same table (test/vectors/keepalive.txt).
//
// Today the daemon pings every 5 s and the dial calls 15 s of silence a dead daemon; the daemon calls 20 s of silence a dead dial. An
// optional `welcome.keepaliveMs` (K) moves the ping to K and scales both windows with it: dial 3 x K, daemon 3 x K + 5 s. Absent, the
// numbers are the old ones. K is clamped to 5 s .. 60 s, so the setting can only slow the ping down, never speed it up.
//
// The daemon's window is the later one on purpose (the old 20 s against 15 s): a single late message cannot trip both sides at once and
// have each conclude the other left.
#include <stdint.h>

#define KEEPALIVE_MIN_MS 5000u
#define KEEPALIVE_MAX_MS 60000u
#define KEEPALIVE_DEFAULT_MS KEEPALIVE_MIN_MS   // the ping it has always been
#define KEEPALIVE_SILENCE_FACTOR 3u             // the dial waits this many pings
#define KEEPALIVE_DAEMON_EXTRA_MS 5000u         // and the daemon waits that long again

// `raw` is the JSON number of `welcome.keepaliveMs`; pass NAN (or any value below the minimum) when the field is absent or not a number.
// No libm: a NaN is the only value not equal to itself.
static inline uint32_t keepalive_effective_ms(double raw)
{
    if (raw != raw) return KEEPALIVE_DEFAULT_MS;
    if (raw <= (double)KEEPALIVE_MIN_MS) return KEEPALIVE_MIN_MS;
    if (raw >= (double)KEEPALIVE_MAX_MS) return KEEPALIVE_MAX_MS;
    return (uint32_t)(raw + 0.5);
}

static inline uint32_t keepalive_dial_silence_ms(uint32_t keepalive_ms) { return KEEPALIVE_SILENCE_FACTOR * keepalive_ms; }
static inline uint32_t keepalive_daemon_silence_ms(uint32_t keepalive_ms) { return KEEPALIVE_SILENCE_FACTOR * keepalive_ms + KEEPALIVE_DAEMON_EXTRA_MS; }
