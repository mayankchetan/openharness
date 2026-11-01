// The BLE link's state, its deadlines and its accept policy, as pure logic (task B4a of the phone bridge): no IDF, no clock, no random source. The
// twin of net_state.c for the phone mode. ble_link.c (B4b) owns the one live instance and feeds it events; test/test_ble_state.c runs the table.
// The wire rules are docs/specs/cable-ble-link.md; the numbers below are the `const` lines of test/vectors/ble_status.txt, and the host test fails
// if they differ.
//
//   OFF           no BLE stack running (USB or Wi-Fi mode, or the phone mode not started).
//   ADVERTISING   advertising, no central yet. BLE_ADV_TIMEOUT_MS without one: NOT_FOUND.
//   NOT_FOUND     advertising stopped with nobody there ("Phone not found"). It does not restart by itself: START begins again.
//   SECURING      a central is connected and the link is not encrypted yet. BLE_ENC_DEADLINE_MS from the connect, then the link is dropped.
//   CONNECTED     the link is encrypted (and bonded); waiting for the phone to open the L2CAP channel. No deadline of ours.
//   CHANNEL_UP    the channel is open and the TLS handshake runs. BLE_TLS_DEADLINE_MS from the open, then the channel is closed.
//   TLS_UP        TLS is up; cable frames flow. (The session layer says "connected" separately, on `welcome`.)
//   AUTH_FAILED   the daemon answered the handshake with an alert three times in a row: the key is refused. The glue drops the link; LINK_DOWN
//                 advertises again, after the refused back-off ladder (net_state.h) rather than at once.
//
// The plan's list was OFF, ADVERTISING, CONNECTED, SECURING, CHANNEL_UP, TLS_UP. Two states are added, because the screen needs to say them:
// NOT_FOUND (the 30 s timeout) and AUTH_FAILED. CONNECTED is "encrypted, waiting for the channel".
//
// ACL link lost (LINK_DOWN) is ADVERTISING again from every connected state: the phone reopens the link when it is back (spec, relay rule 5).
// A channel that closes (CHANNEL_CLOSED, TLS_FAIL) keeps the link and the bond: CONNECTED, and the phone may open a new channel.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "net_state.h"

#define BLE_ADV_TIMEOUT_MS   30000u
#define BLE_ENC_DEADLINE_MS  3000u
#define BLE_TLS_DEADLINE_MS  5000u

typedef enum {
    BLE_ST_OFF = 0,
    BLE_ST_ADVERTISING,
    BLE_ST_NOT_FOUND,
    BLE_ST_SECURING,
    BLE_ST_CONNECTED,
    BLE_ST_CHANNEL_UP,
    BLE_ST_TLS_UP,
    BLE_ST_AUTH_FAILED,
    BLE_ST_COUNT
} ble_state_t;

// START is 0 on purpose: BLE_EV_NONE (-1) is "no event" and never enters the table.
typedef enum {
    BLE_EV_NONE = -1,
    BLE_EV_START = 0,        // the phone mode begins (or begins again after NOT_FOUND)
    BLE_EV_STOP,             // the phone mode ends
    BLE_EV_ADV_TIMEOUT,      // BLE_ADV_TIMEOUT_MS of advertising with no central
    BLE_EV_LINK_UP,          // a central connected (ACL)
    BLE_EV_LINK_DOWN,        // the ACL link ended
    BLE_EV_ENCRYPTED,        // the link is encrypted (and, in the product, bonded)
    BLE_EV_ENC_TIMEOUT,      // BLE_ENC_DEADLINE_MS after the connect, not encrypted: the glue drops the link
    BLE_EV_CHANNEL_OPEN,     // the L2CAP channel was accepted (the accept policy said yes)
    BLE_EV_CHANNEL_CLOSED,   // the channel closed (either side); the link stays
    BLE_EV_TLS_UP,           // the handshake finished
    BLE_EV_TLS_FAIL,         // the handshake failed (an ordinary failure, a first or second alert, or the deadline), or an up link failed
    BLE_EV_TLS_REFUSED,      // the third alert in a row
    BLE_EV_COUNT
} ble_event_t;

// The next state. An event that cannot happen in a state leaves it alone, and so does anything out of range; a state out of range reads as OFF.
ble_state_t ble_state_next(ble_state_t state, ble_event_t event);
const char *ble_state_name(ble_state_t state);       // "off", "advertising", "not_found", "securing", "connected", "channel_up", "tls_up", "auth_failed"; "?" out of range

// ── the instance ────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    ble_state_t   state;
    uint64_t      since_ms;   // when the state was entered (the deadlines count from here)
    uint64_t      last_ms;    // the newest clock reading: a clock that steps back is held at it
    net_refusal_t refusals;   // alerts in a row (net_state.h)
} ble_t;

void ble_init(ble_t *b);   // OFF, zeroed
// Fold one event in. True when the state changed (and since_ms is now). START and TLS_UP clear the run of refusals; LINK_DOWN does not (the refused
// ladder is about the daemon refusing this dial, not about which link it came over).
bool ble_post(ble_t *b, ble_event_t event, uint64_t now_ms);

// How long a state may last, 0 for no deadline. ADVERTISING, SECURING and CHANNEL_UP have one; nothing else does.
uint32_t ble_deadline_ms(ble_state_t state);
// The event the deadline of the current state would post at `now_ms` (>=, so 2.999 s is not 3 s): ADV_TIMEOUT, ENC_TIMEOUT, or TLS_FAIL for the
// handshake. BLE_EV_NONE when nothing is due. It stays due until the caller posts it; a TLS_FAIL from here goes through ble_handshake_failed(b, false).
ble_event_t ble_poll(const ble_t *b, uint64_t now_ms);

// One failed handshake, in CHANNEL_UP: folds it into the run of refusals (net_handshake_event), posts TLS_FAIL, or TLS_REFUSED on the third alert in a row,
// and returns the event it posted. `refused` = the daemon answered with a fatal alert (net_hs_class.h). Outside CHANNEL_UP no handshake is running:
// BLE_EV_NONE, nothing counted.
ble_event_t ble_handshake_failed(ble_t *b, bool refused, uint64_t now_ms);

// ── admission ───────────────────────────────────────────────────────────────────────────────────────
// Only an encrypted, bonded peer gets the channel; one channel at a time. `dev_unbonded` is the build flag BLE_DEV_ALLOW_UNBONDED (off in every product
// config; TLS-PSK still authenticates both ends): it waives the bond and the encryption, never the one-channel rule.
typedef enum {
    BLE_ACCEPT_OK = 0,
    BLE_ACCEPT_REFUSE_ENC,      // not encrypted, or encrypted without a bond: refuse with "insufficient encryption"
    BLE_ACCEPT_REFUSE_SECOND,   // a channel is already open
} ble_accept_t;

ble_accept_t ble_accept_check(bool encrypted, bool bonded, bool dev_unbonded, bool channel_open);
// Does the link need to become encrypted at all? With the dev flag it does not, so the glue posts ENCRYPTED at the connect and the 3 s deadline never fires.
bool ble_encryption_required(bool dev_unbonded);
