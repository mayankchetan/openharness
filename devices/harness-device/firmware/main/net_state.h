// The Wi-Fi mode's link state and its reconnect timing, as pure logic: no IDF, no clock, no random source (the caller passes a random word).
// Host-tested in test/test_net_state.c; net_link.c owns the one live instance and feeds it events.
//
//   OFF                 no radio (USB mode, or Wi-Fi mode not started).
//   JOINING             working on it: scanning, associating, waiting for DHCP, connecting to the daemon, shaking hands.
//   NO_IP               associated, but DHCP gave no address in time (or the address was lost).
//   DAEMON_UNREACHABLE  there is an address, but the daemon's TCP port did not answer, or the handshake did not finish.
//   AUTH_FAILED         the daemon answered and refused the handshake: wrong key, or this dial is unknown to it. Retried slowly.
//   UP                  TLS is up; cable frames flow. (The session layer says "connected" separately, on `welcome`.)
//
// The same ladder serves a lost access point and a lost TCP link ("Wi-Fi loss and TCP loss share the ladder", cable-wifi-link.md section 4.3).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_OFF = 0,
    NET_JOINING,
    NET_NO_IP,
    NET_DAEMON_UNREACHABLE,
    NET_AUTH_FAILED,
    NET_UP,
    NET_STATE_COUNT
} net_state_t;

typedef enum {
    NET_EV_START = 0,    // Wi-Fi mode begins
    NET_EV_STOP,         // Wi-Fi mode ends
    NET_EV_WIFI_DOWN,    // the access point was lost, or never joined
    NET_EV_GOT_IP,       // DHCP answered
    NET_EV_LOST_IP,      // the address is gone while still associated
    NET_EV_IP_TIMEOUT,   // associated, and still no address after the wait
    NET_EV_TCP_FAIL,     // the daemon's port did not accept a connection
    NET_EV_TLS_FAIL,     // TCP connected, the handshake did not finish (timeout, noise)
    NET_EV_TLS_REFUSED,  // TCP connected, the daemon refused the handshake (alert, reset, bad MAC)
    NET_EV_LINK_UP,      // the handshake finished
    NET_EV_LINK_DOWN,    // an established link ended
    NET_EV_COUNT
} net_event_t;

// The next state. An event that cannot happen in a state (a TCP failure with no address) leaves it alone, and so does anything out of range.
net_state_t net_state_next(net_state_t state, net_event_t event);

const char *net_state_name(net_state_t state);      // for log lines: "joining", "auth_failed", ...
// What the glass says under the wordmark, or NULL when there is nothing to say (OFF, UP). At most 24 characters.
const char *net_state_caption(net_state_t state);

// ── reconnect timing ────────────────────────────────────────────────────────────────────────────────
// Ordinary ladder 1, 2, 4, 8, 16, 30, 60 s (the last step repeats). A refused handshake waits longer, 30, 60, 120, 300 s: retrying a wrong
// key every second helps nobody and floods the daemon's log. Each delay is the step plus or minus a quarter, chosen by a random word.
typedef struct { uint8_t attempt; } net_backoff_t;
#define NET_BACKOFF_MAX_ATTEMPT 100   // the counter stops here; both ladders ended long before

void     net_backoff_reset(net_backoff_t *b);
uint32_t net_backoff_base_ms(uint32_t attempt, bool refused);
uint32_t net_backoff_jitter_ms(uint32_t base_ms, uint32_t random_word);   // in [3/4 base, 5/4 base]
// The delay for the current attempt (with jitter), then the attempt counter advances.
uint32_t net_backoff_next_ms(net_backoff_t *b, bool refused, uint32_t random_word);
// A link that stayed up this long earns a fresh ladder; one that dropped sooner (a daemon that accepts and hangs up) keeps climbing.
bool     net_backoff_link_earned_reset(uint32_t up_ms);
