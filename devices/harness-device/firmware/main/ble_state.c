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
//                         START STOP ADV_TO LINK_UP LINK_DOWN ENC ENC_TO CH_OPEN CH_CLOSED TLS_UP TLS_FAIL TLS_REFUSED
static const uint8_t NEXT[BLE_ST_COUNT][BLE_EV_COUNT] = {
    /* OFF         */ { A,    O,   O,     O,      O,        O,  O,     O,      O,        O,     O,       O },
    /* ADVERTISING */ { A,    O,   F,     S,      A,        A,  A,     A,      A,        A,     A,       A },
    /* NOT_FOUND   */ { A,    O,   F,     F,      F,        F,  F,     F,      F,        F,     F,       F },
    /* SECURING    */ { S,    O,   S,     S,      A,        C,  A,     S,      S,        S,     S,       S },
    /* CONNECTED   */ { C,    O,   C,     C,      A,        C,  C,     H,      C,        C,     C,       C },
    /* CHANNEL_UP  */ { H,    O,   H,     H,      A,        H,  H,     H,      C,        U,     C,       R },
    /* TLS_UP      */ { U,    O,   U,     U,      A,        U,  U,     U,      C,        U,     C,       U },
    /* AUTH_FAILED */ { R,    O,   R,     R,      A,        R,  R,     R,      R,        R,     R,       R },
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
    case BLE_ST_OFF:
    case BLE_ST_NOT_FOUND:
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
    const uint32_t limit = ble_deadline_ms(b->state);
    if (limit == 0 || now_ms < b->since_ms || now_ms - b->since_ms < limit) return BLE_EV_NONE;
    switch (b->state) {
    case BLE_ST_ADVERTISING: return BLE_EV_ADV_TIMEOUT;
    case BLE_ST_SECURING:    return BLE_EV_ENC_TIMEOUT;
    case BLE_ST_CHANNEL_UP:  return BLE_EV_TLS_FAIL;
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

ble_accept_t ble_accept_check(bool encrypted, bool bonded, bool dev_unbonded, bool channel_open)
{
    if (channel_open) return BLE_ACCEPT_REFUSE_SECOND;
    if (dev_unbonded) return BLE_ACCEPT_OK;
    return encrypted && bonded ? BLE_ACCEPT_OK : BLE_ACCEPT_REFUSE_ENC;
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
