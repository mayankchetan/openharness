// The BLE link's state, its deadlines and its accept policy, as pure logic (task B4a of the phone bridge): no IDF, no clock, no random source. The
// twin of net_state.c for the phone mode. ble_link.c (B4b) owns the one live instance and feeds it events; test/test_ble_state.c runs the table.
// The wire rules are docs/specs/cable-ble-link.md; the numbers below are the `const` lines of test/vectors/ble_status.txt, and the host test fails
// if they differ.
//
//   OFF           no BLE stack running (USB or Wi-Fi mode, or the phone mode not started).
//   ADVERTISING   advertising, no central yet. BLE_ADV_TIMEOUT_MS without one: NOT_FOUND.
//   NOT_FOUND     advertising stopped with nobody there ("Phone not found"). BLE_RETRY_MS later ble_poll says START (advertise again), and the
//                 glue posts START on any screen wake: 30 s on the air, 120 s off, so the radio is on a fifth of the time while no phone comes.
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
#include <stddef.h>
#include <stdint.h>

#include "net_state.h"

#define BLE_ADV_TIMEOUT_MS   30000u
#define BLE_RETRY_MS         120000u   // NOT_FOUND to advertising again, with no wake
#define BLE_ENC_DEADLINE_MS  3000u
#define BLE_IDLE_CAP_MS      10000u    // CONNECTED, waiting for a channel, for a peer that is not authenticated-bonded
#define BLE_CHURN_LIMIT      3u        // channels that opened and closed without reaching TLS_UP, per link, then the link is ended
#define BLE_COOLDOWN_MS      10000u    // flat wait before advertising again after an abusive or unproven peer (no escalation)
#define BLE_KEY_SIZE_MIN     16u       // bytes of the link key
#define BLE_PAIR_WINDOW_MS   120000u   // the window the user opens to pair a phone (B5 opens it)
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
    BLE_EV_IDLE_TIMEOUT,     // CONNECTED for BLE_IDLE_CAP_MS with a peer that is not authenticated-bonded: the glue drops the link
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
    bool          trusted;    // this link is authenticated, bonded and long-keyed (ble_sec_trusted); set by the glue, cleared at every new link
    uint8_t       churn;      // channels of this link that ended without reaching TLS_UP (saturates); cleared by TLS_UP and by a new link
} ble_t;

void ble_init(ble_t *b);   // OFF, zeroed
// Fold one event in. True when the state changed (and since_ms is now). START and TLS_UP clear the run of refusals; LINK_DOWN does not (the refused
// ladder is about the daemon refusing this dial, not about which link it came over).
bool ble_post(ble_t *b, ble_event_t event, uint64_t now_ms);

// The glue says this link passed the security rule (ble_sec_trusted) before it posts ENCRYPTED. Only a trusted peer may sit in CONNECTED without limit.
void ble_set_trusted(ble_t *b, bool trusted);
// This link has opened and lost BLE_CHURN_LIMIT channels without one reaching TLS_UP: the glue ends it and waits BLE_COOLDOWN_MS before advertising.
bool ble_churn_exceeded(const ble_t *b);
// How long to stay silent after a refused key (three alerts in a row): a peer that is not trusted gets the flat BLE_COOLDOWN_MS, always, so an attacker
// cannot walk the ladder up; a trusted one gets the refused ladder of net_state.h (30 s to 5 min, with jitter from `random_word`). NULL ladder: flat.
uint32_t ble_refused_wait_ms(bool trusted, net_backoff_t *ladder, uint32_t random_word);

// How long a state may last, 0 for no deadline. ADVERTISING, SECURING, CHANNEL_UP and NOT_FOUND (the retry) have one; nothing else does.
uint32_t ble_deadline_ms(ble_state_t state);
// The event the deadline of the current state would post at `now_ms` (>=, so 2.999 s is not 3 s): ADV_TIMEOUT, ENC_TIMEOUT, TLS_FAIL for the
// handshake, or START for the retry out of NOT_FOUND. BLE_EV_NONE when nothing is due. It stays due until the caller posts it; a TLS_FAIL from here goes through ble_handshake_failed(b, false).
ble_event_t ble_poll(const ble_t *b, uint64_t now_ms);

// One failed handshake, in CHANNEL_UP: folds it into the run of refusals (net_handshake_event), posts TLS_FAIL, or TLS_REFUSED on the third alert in a row,
// and returns the event it posted. `refused` = the daemon answered with a fatal alert (net_hs_class.h). Outside CHANNEL_UP no handshake is running:
// BLE_EV_NONE, nothing counted.
ble_event_t ble_handshake_failed(ble_t *b, bool refused, uint64_t now_ms);

// ── what goes on the air (docs/specs/cable-ble-link.md section 2; the bytes of test/vectors/ble_status.txt) ──────────────────────────────────────
// The 128-bit service UUID e70301f0-bbe6-4d70-801c-657c8e99501d, last byte first, as the scan response and the GATT table carry it.
extern const uint8_t BLE_SERVICE_UUID_LE[16];
// "harness-dial-<mac2>": the last two bytes of the BASE MAC (the PSK identity, "B8:1F:3F:CA:E5:58", either case in) as four lower-case hex digits.
// `cap` must hold 17 characters and the NUL (18). False, and out[0] = 0 when it can, for anything that is not six hex pairs with colons.
bool ble_adv_name(const char *mac, char *out, size_t cap);
// The advertising packet: flags 02 01 06, then the complete local name (AD type 0x09): 22 bytes. 0 when the MAC is bad or `cap` is under 22.
size_t ble_adv_data(const char *mac, uint8_t *out, size_t cap);
// The scan response: the service UUID as a complete 128-bit list (AD type 0x07): 18 bytes. 0 when `cap` is under 18.
size_t ble_scan_rsp_data(uint8_t *out, size_t cap);

// ── admission ───────────────────────────────────────────────────────────────────────────────────────
// What the link says about itself (NimBLE's sec_state of the connection).
typedef struct { bool encrypted, bonded, authenticated; uint8_t key_size; } ble_sec_t;
// The product rule: encrypted AND bonded AND authenticated (numeric comparison, not Just Works) AND a key of at least BLE_KEY_SIZE_MIN bytes. A peer
// that says NoInputNoOutput gets Just Works and a bond with authenticated = 0; it is not trusted. NULL: not trusted.
bool ble_sec_trusted(const ble_sec_t *sec);

// Only a trusted peer gets the channel; one channel at a time. `dev_unbonded` is the build flag BLE_DEV_ALLOW_UNBONDED (off in every product
// config; TLS-PSK still authenticates both ends): it waives the whole security rule, never the one-channel rule.
typedef enum {
    BLE_ACCEPT_OK = 0,
    BLE_ACCEPT_REFUSE_ENC,      // not encrypted, or encrypted without a bond: refuse with "insufficient encryption"
    BLE_ACCEPT_REFUSE_SECOND,   // a channel is already open
} ble_accept_t;

ble_accept_t ble_accept_check(const ble_sec_t *sec, bool dev_unbonded, bool channel_open);
// Does the link need to become encrypted at all? With the dev flag it does not, so the glue posts ENCRYPTED at the connect and the 3 s deadline never fires.
bool ble_encryption_required(bool dev_unbonded);

// ── the two characteristics the phone writes (docs/specs/cable-ble-link.md section 5; B4c) ────────────────────────────────────────────────
// The checks are pure so the host test runs them against the `hint` and `status` lines of test/vectors/ble_status.txt. Each returns 0 (accepted: *out is the
// value) or the ATT error to send back, and leaves *out alone on an error. `out` may be NULL to only ask.
#define BLE_HINT_ERR_LEN    0x0Du   // the length is not 1 (ATT: invalid attribute value length)
#define BLE_HINT_ERR_RANGE  0x80u   // a hint above 1 (ATT: application error, value out of range)
uint8_t ble_hint_check(const uint8_t *data, size_t len, uint8_t *out);          // home hint: one byte, 0 or 1
uint8_t ble_relay_status_check(const uint8_t *data, size_t len, uint8_t *out);  // relay status: one byte, any value (an unknown one is shown, not an error)
// "idle", "tunnel_connecting", "tunnel_up", "token_rejected", "phone_no_network"; anything else "unknown".
const char *ble_relay_status_name(uint8_t value);

// ── bonds ───────────────────────────────────────────────────────────────────────────────────────────
// A new bond is made only inside an explicit pairing window, and never by evicting an old one. The window is closed by default and closes by itself at
// BLE_PAIR_WINDOW_MS (120.0 s); B5 opens it from the Settings row. A clock that stepped back before the window opened reads as closed.
typedef struct { bool open; uint64_t from_ms, until_ms; } ble_pair_window_t;
void ble_pair_init(ble_pair_window_t *w);                       // closed
void ble_pair_open(ble_pair_window_t *w, uint64_t now_ms);      // open (again) for BLE_PAIR_WINDOW_MS from now
void ble_pair_close(ble_pair_window_t *w);
bool ble_pair_is_open(const ble_pair_window_t *w, uint64_t now_ms);
// May a pairing that wants to save a bond do so: the window is open AND the store has room (`store_full` = the stack asked to evict). Full means no.
bool ble_bond_allowed(const ble_pair_window_t *w, uint64_t now_ms, bool store_full);
