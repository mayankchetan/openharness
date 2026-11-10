#include "ble_state.h"

#include <stddef.h>
#include <string.h>

// Rows are states, columns are events, in the order of ble_event_t. The one-letter names are for this table only.
#define O BLE_ST_OFF
#define A BLE_ST_ADVERTISING
#define F BLE_ST_NOT_FOUND
#define S BLE_ST_SECURING
#define C BLE_ST_CONNECTED
#define H BLE_ST_CHANNEL_UP
#define U BLE_ST_TLS_UP
#define R BLE_ST_AUTH_FAILED
//                         START STOP ADV_TO LINK_UP LINK_DOWN ENC ENC_TO CH_OPEN CH_CLOSED TLS_UP TLS_FAIL TLS_REFUSED IDLE_TO
static const uint8_t NEXT[BLE_ST_COUNT][BLE_EV_COUNT] = {
    /* OFF         */ { A,    O,   O,     O,      O,        O,  O,     O,      O,        O,     O,       O, O },
    /* ADVERTISING */ { A,    O,   F,     S,      A,        A,  A,     A,      A,        A,     A,       A, A },
    /* NOT_FOUND   */ { A,    O,   F,     S,      F,        F,  F,     F,      F,        F,     F,       F, F },
    /* SECURING    */ { S,    O,   S,     S,      A,        C,  A,     S,      S,        S,     S,       S, S },
    /* CONNECTED   */ { C,    O,   C,     C,      A,        C,  C,     H,      C,        C,     C,       C, A },
    /* CHANNEL_UP  */ { H,    O,   H,     H,      A,        H,  H,     H,      C,        U,     C,       R, H },
    /* TLS_UP      */ { U,    O,   U,     U,      A,        U,  U,     U,      C,        U,     C,       U, U },
    /* AUTH_FAILED */ { R,    O,   R,     R,      A,        R,  R,     R,      R,        R,     R,       R, R },
};
#undef O
#undef A
#undef F
#undef S
#undef C
#undef H
#undef U
#undef R

ble_state_t ble_state_next(ble_state_t state, ble_event_t event)
{
    if ((unsigned)state >= BLE_ST_COUNT) return BLE_ST_OFF;
    if (event < 0 || (unsigned)event >= BLE_EV_COUNT) return state;
    return (ble_state_t)NEXT[state][event];
}

const char *ble_state_name(ble_state_t state)
{
    switch (state) {
    case BLE_ST_OFF:         return "off";
    case BLE_ST_ADVERTISING: return "advertising";
    case BLE_ST_NOT_FOUND:   return "not_found";
    case BLE_ST_SECURING:    return "securing";
    case BLE_ST_CONNECTED:   return "connected";
    case BLE_ST_CHANNEL_UP:  return "channel_up";
    case BLE_ST_TLS_UP:      return "tls_up";
    case BLE_ST_AUTH_FAILED: return "auth_failed";
    case BLE_ST_COUNT:       break;
    }
    return "?";
}

void ble_init(ble_t *b)
{
    if (!b) return;
    memset(b, 0, sizeof *b);
}

bool ble_post(ble_t *b, ble_event_t event, uint64_t now_ms)
{
    if (!b) return false;
    if (now_ms < b->last_ms) now_ms = b->last_ms;   // a clock that steps back neither fires nor rewinds a timer
    b->last_ms = now_ms;
    if (event == BLE_EV_START || event == BLE_EV_TLS_UP) net_refusal_reset(&b->refusals);
    const ble_state_t next = ble_state_next(b->state, event);
    if (next == b->state) return false;
    // what the change means for the abuse counters
    if (event == BLE_EV_LINK_UP) { b->churn = 0; b->trusted = false; }                       // a new link starts clean
    if (event == BLE_EV_LINK_DOWN) b->trusted = false;
    if (event == BLE_EV_TLS_UP) b->churn = 0;                                                // proven: the peer holds the key
    if (b->state == BLE_ST_CHANNEL_UP && (event == BLE_EV_CHANNEL_CLOSED || event == BLE_EV_TLS_FAIL || event == BLE_EV_TLS_REFUSED) && b->churn < 255) b->churn++;
    b->prompting = false;   // any change of state ends the wait for the number
    b->adv_hold = false;    // and the advertising hold
    b->state = next;
    b->since_ms = now_ms;
    return true;
}

uint32_t ble_deadline_ms(ble_state_t state)
{
    switch (state) {
    case BLE_ST_ADVERTISING: return BLE_ADV_TIMEOUT_MS;
    case BLE_ST_SECURING:    return BLE_ENC_DEADLINE_MS;
    case BLE_ST_CHANNEL_UP:  return BLE_TLS_DEADLINE_MS;
    case BLE_ST_NOT_FOUND:   return BLE_RETRY_MS;
    case BLE_ST_OFF:
    case BLE_ST_CONNECTED:
    case BLE_ST_TLS_UP:
    case BLE_ST_AUTH_FAILED:
    case BLE_ST_COUNT:       break;
    }
    return 0;
}

ble_event_t ble_poll(const ble_t *b, uint64_t now_ms)
{
    if (!b) return BLE_EV_NONE;
    uint32_t limit = ble_deadline_ms(b->state);
    if (b->state == BLE_ST_CONNECTED && !b->trusted) limit = BLE_IDLE_CAP_MS;   // only a trusted peer may wait for ever
    if (b->state == BLE_ST_SECURING && b->prompting) return BLE_EV_NONE;          // the number is on the screen: the person is comparing it (B5)
    if (b->state == BLE_ST_ADVERTISING && b->adv_hold) return BLE_EV_NONE;        // the pairing window is open: stay on the air (item 4a)
    if (limit == 0 || now_ms < b->since_ms || now_ms - b->since_ms < limit) return BLE_EV_NONE;
    switch (b->state) {
    case BLE_ST_ADVERTISING: return BLE_EV_ADV_TIMEOUT;
    case BLE_ST_SECURING:    return BLE_EV_ENC_TIMEOUT;
    case BLE_ST_CHANNEL_UP:  return BLE_EV_TLS_FAIL;
    case BLE_ST_NOT_FOUND:   return BLE_EV_START;
    case BLE_ST_CONNECTED:   return BLE_EV_IDLE_TIMEOUT;
    default:                 break;
    }
    return BLE_EV_NONE;
}

ble_event_t ble_handshake_failed(ble_t *b, bool refused, uint64_t now_ms)
{
    if (!b || b->state != BLE_ST_CHANNEL_UP) return BLE_EV_NONE;
    const net_event_t folded = net_handshake_event(&b->refusals, refused);
    const ble_event_t event = folded == NET_EV_TLS_REFUSED ? BLE_EV_TLS_REFUSED : BLE_EV_TLS_FAIL;
    ble_post(b, event, now_ms);
    return event;
}

bool ble_sec_trusted(const ble_sec_t *sec)
{
    return sec && sec->encrypted && sec->bonded && sec->authenticated && sec->key_size >= BLE_KEY_SIZE_MIN;
}

ble_accept_t ble_accept_check(const ble_sec_t *sec, bool dev_unbonded, bool channel_open)
{
    if (channel_open) return BLE_ACCEPT_REFUSE_SECOND;
    if (dev_unbonded) return BLE_ACCEPT_OK;
    return ble_sec_trusted(sec) ? BLE_ACCEPT_OK : BLE_ACCEPT_REFUSE_ENC;
}

void ble_set_trusted(ble_t *b, bool trusted)
{
    if (b) b->trusted = trusted;
}

bool ble_churn_exceeded(const ble_t *b)
{
    return b && b->churn >= BLE_CHURN_LIMIT;
}

uint32_t ble_refused_wait_ms(bool trusted, net_backoff_t *ladder, uint32_t random_word)
{
    if (!trusted || !ladder) return BLE_COOLDOWN_MS;
    return net_backoff_next_ms(ladder, true, random_word);
}

void ble_pair_init(ble_pair_window_t *w)
{
    if (w) memset(w, 0, sizeof *w);
}

void ble_pair_open(ble_pair_window_t *w, uint64_t now_ms)
{
    if (!w) return;
    w->open = true;
    w->from_ms = now_ms;
    w->until_ms = now_ms + BLE_PAIR_WINDOW_MS;
}

void ble_pair_close(ble_pair_window_t *w)
{
    if (w) w->open = false;
}

bool ble_pair_is_open(const ble_pair_window_t *w, uint64_t now_ms)
{
    return w && w->open && now_ms >= w->from_ms && now_ms < w->until_ms;
}

uint32_t ble_pair_left_ms(const ble_pair_window_t *w, uint64_t now_ms)
{
    return ble_pair_is_open(w, now_ms) ? (uint32_t)(w->until_ms - now_ms) : 0;
}

bool ble_bond_allowed(const ble_pair_window_t *w, uint64_t now_ms, bool store_full)
{
    return !store_full && ble_pair_is_open(w, now_ms);
}

// ── the characteristics the phone writes ────────────────────────────────────────────────────────────
uint8_t ble_hint_check(const uint8_t *data, size_t len, uint8_t *out)
{
    if (!data || len != 1) return BLE_HINT_ERR_LEN;
    if (data[0] > 1) return BLE_HINT_ERR_RANGE;
    if (out) *out = data[0];
    return 0;
}

uint8_t ble_relay_status_check(const uint8_t *data, size_t len, uint8_t *out)
{
    if (!data || len != 1) return BLE_HINT_ERR_LEN;
    if (out) *out = data[0];
    return 0;
}

const char *ble_relay_status_name(uint8_t value)
{
    switch (value) {
    case 0x00: return "idle";
    case 0x01: return "tunnel_connecting";
    case 0x02: return "tunnel_up";
    case 0x03: return "token_rejected";
    case 0x04: return "phone_no_network";
    default:   return "unknown";
    }
}

void ble_set_prompting(ble_t *b, bool prompting)
{
    if (b) b->prompting = prompting;
}

bool ble_pair_auto_open_wanted(bool phone_mode_on, unsigned bonds)
{
    return phone_mode_on && bonds == 0;
}

bool ble_pair_rearm_wanted(const ble_pair_window_t *w, uint64_t now_ms, bool phone_mode_on, unsigned bonds)
{
    return ble_pair_auto_open_wanted(phone_mode_on, bonds) && !ble_pair_is_open(w, now_ms);
}

void ble_adv_hold_update(ble_t *b, const ble_pair_window_t *w, uint64_t now_ms)
{
    if (!b || b->state != BLE_ST_ADVERTISING) return;
    b->adv_hold = ble_pair_is_open(w, now_ms);
}

bool ble_peer_may_hold_bond(bool found, unsigned bonds, uint8_t addr_type, const uint8_t addr_le[6])
{
    if (found) return true;
    if (bonds == 0 || addr_type != 1 || !addr_le) return false;
    return (addr_le[5] & 0xC0) == 0x40;
}

bool ble_pair_stays_open_after_no(bool phone_mode_on, unsigned bonds)
{
    return ble_pair_auto_open_wanted(phone_mode_on, bonds);
}

void ble_enc_hold_update(ble_t *b, const ble_pair_window_t *w, uint64_t now_ms, bool number_shown)
{
    if (!b || b->state != BLE_ST_SECURING) return;   // the hold is for SECURING only; any other state has no 3 s deadline to hold
    // The window holds the deadline for BLE_ENC_HOLD_MAX_MS of this link (since_ms is when SECURING began, which is the connect), and no longer; a number
    // on the screen holds on its own bound.
    const uint64_t age = now_ms >= b->since_ms ? now_ms - b->since_ms : 0;
    b->prompting = number_shown || (ble_pair_is_open(w, now_ms) && age < BLE_ENC_HOLD_MAX_MS);
}

bool ble_security_request_wanted(const ble_pair_window_t *w, uint64_t now_ms, bool peer_bonded)
{
    return peer_bonded || ble_pair_is_open(w, now_ms);
}

ble_numcmp_t ble_numcmp_request(const ble_pair_window_t *w, uint64_t now_ms)
{
    return ble_pair_is_open(w, now_ms) ? BLE_NUMCMP_SHOW : BLE_NUMCMP_REFUSE;
}

ble_pair_verdict_t ble_numcmp_verdict(const ble_pair_window_t *w, uint64_t now_ms, int answer)
{
    if (!ble_pair_is_open(w, now_ms)) return BLE_PAIR_REJECT;   // closed by hand, expired, or a clock that stepped back: the safe side
    if (answer == 0) return BLE_PAIR_REJECT;
    if (answer == 1) return BLE_PAIR_ACCEPT;
    return BLE_PAIR_WAIT;
}

ble_pair_verdict_t ble_prompt_verdict(const ble_pair_window_t *w, uint64_t shown_ms, uint64_t now_ms, int answer)
{
    if (now_ms < shown_ms || now_ms - shown_ms >= BLE_PROMPT_MAX_MS) return BLE_PAIR_REJECT;
    return ble_numcmp_verdict(w, now_ms, answer);
}

bool ble_bond_unwanted(const ble_sec_t *sec)
{
    return sec && sec->bonded && !ble_sec_trusted(sec);
}

bool ble_encryption_required(bool dev_unbonded) { return !dev_unbonded; }

// ── what goes on the air ────────────────────────────────────────────────────────────────────────────
const uint8_t BLE_SERVICE_UUID_LE[16] = {0x1d, 0x50, 0x99, 0x8e, 0x7c, 0x65, 0x1c, 0x80, 0x70, 0x4d, 0xe6, 0xbb, 0xf0, 0x01, 0x03, 0xe7};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#define BLE_NAME_PREFIX "harness-dial-"
#define BLE_NAME_LEN    17   // 13 + 4

bool ble_adv_name(const char *mac, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!mac || !out || cap < BLE_NAME_LEN + 1) return false;
    // exactly "XX:XX:XX:XX:XX:XX"
    if (strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (mac[i] != ':') return false; }
        else if (hex_digit(mac[i]) < 0) return false;
    }
    static const char LOWER[] = "0123456789abcdef";
    memcpy(out, BLE_NAME_PREFIX, 13);
    // the last two bytes: characters 12-13 and 15-16
    out[13] = LOWER[hex_digit(mac[12])];
    out[14] = LOWER[hex_digit(mac[13])];
    out[15] = LOWER[hex_digit(mac[15])];
    out[16] = LOWER[hex_digit(mac[16])];
    out[17] = 0;
    return true;
}

size_t ble_adv_data(const char *mac, uint8_t *out, size_t cap)
{
    char name[BLE_NAME_LEN + 1];
    if (!out || cap < 22 || !ble_adv_name(mac, name, sizeof name)) return 0;
    size_t n = 0;
    out[n++] = 0x02; out[n++] = 0x01; out[n++] = 0x06;                     // flags: general discoverable, no BR/EDR
    out[n++] = (uint8_t)(BLE_NAME_LEN + 1); out[n++] = 0x09;               // complete local name
    memcpy(out + n, name, BLE_NAME_LEN);
    return n + BLE_NAME_LEN;
}

size_t ble_scan_rsp_data(uint8_t *out, size_t cap)
{
    if (!out || cap < 18) return 0;
    out[0] = 17;     // type byte + 16
    out[1] = 0x07;   // complete list of 128-bit service UUIDs
    memcpy(out + 2, BLE_SERVICE_UUID_LE, 16);
    return 18;
}
