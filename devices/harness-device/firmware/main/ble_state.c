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
