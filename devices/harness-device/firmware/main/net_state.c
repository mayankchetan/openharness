#include "net_state.h"

#include <stddef.h>

// Rows are states, columns are events, in the order of net_event_t. The one-letter names are for this table only.
#define O NET_OFF
#define J NET_JOINING
#define N NET_NO_IP
#define D NET_DAEMON_UNREACHABLE
#define A NET_AUTH_FAILED
#define U NET_UP
//                                START STOP WIFI_DOWN GOT_IP LOST_IP IP_TIMEOUT TCP_FAIL TLS_FAIL TLS_REFUSED LINK_UP LINK_DOWN
static const uint8_t NEXT[NET_STATE_COUNT][NET_EV_COUNT] = {
    /* OFF                */ { J,    O,   O,        O,     O,      O,         O,       O,       O,          O,      O },
    /* JOINING            */ { J,    O,   J,        J,     J,      N,         D,       D,       A,          U,      J },
    /* NO_IP              */ { N,    O,   J,        J,     N,      N,         N,       N,       N,          N,      N },
    /* DAEMON_UNREACHABLE */ { D,    O,   J,        D,     N,      D,         D,       D,       A,          U,      D },
    /* AUTH_FAILED        */ { A,    O,   J,        A,     N,      A,         D,       D,       A,          U,      A },
    /* UP                 */ { U,    O,   J,        U,     N,      U,         D,       D,       A,          U,      J },
};
#undef O
#undef J
#undef N
#undef D
#undef A
#undef U

net_state_t net_state_next(net_state_t state, net_event_t event)
{
    if ((unsigned)state >= NET_STATE_COUNT) return NET_OFF;
    if ((unsigned)event >= NET_EV_COUNT) return state;
    return (net_state_t)NEXT[state][event];
}

const char *net_state_name(net_state_t state)
{
    switch (state) {
    case NET_OFF:                return "off";
    case NET_JOINING:            return "joining";
    case NET_NO_IP:              return "no_ip";
    case NET_DAEMON_UNREACHABLE: return "daemon_unreachable";
    case NET_AUTH_FAILED:        return "auth_failed";
    case NET_UP:                 return "up";
    case NET_STATE_COUNT:        break;
    }
    return "?";
}

const char *net_state_caption(net_state_t state)
{
    switch (state) {
    case NET_JOINING:            return "Joining Wi-Fi";
    case NET_NO_IP:              return "Wi-Fi: no address";
    case NET_DAEMON_UNREACHABLE: return "Daemon not reachable";
    case NET_AUTH_FAILED:        return "Key refused: reset dial";   // W14 brings back the USB recovery (the probe); until then a factory reset is the way
    case NET_OFF:
    case NET_UP:
    case NET_STATE_COUNT:        break;
    }
    return NULL;
}

// ── backoff ─────────────────────────────────────────────────────────────────────────────────────────

static const uint32_t LADDER_MS[] = { 1000, 2000, 4000, 8000, 16000, 30000, 60000 };
static const uint32_t REFUSED_MS[] = { 30000, 60000, 120000, 300000 };
#define COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

void net_backoff_reset(net_backoff_t *b) { b->attempt = 0; }

uint32_t net_backoff_base_ms(uint32_t attempt, bool refused)
{
    const uint32_t *ladder = refused ? REFUSED_MS : LADDER_MS;
    const uint32_t n = refused ? COUNT(REFUSED_MS) : COUNT(LADDER_MS);
    return ladder[attempt < n ? attempt : n - 1];
}

uint32_t net_backoff_jitter_ms(uint32_t base_ms, uint32_t random_word)
{
    // base - base/4 plus a value in [0, base/2]: the band is symmetric about the step, at most 5/4 of it. 300 s x 5/4 is far from 2^32.
    return base_ms - base_ms / 4 + random_word % (base_ms / 2 + 1);
}

uint32_t net_backoff_next_ms(net_backoff_t *b, bool refused, uint32_t random_word)
{
    const uint32_t delay = net_backoff_jitter_ms(net_backoff_base_ms(b->attempt, refused), random_word);
    if (b->attempt < NET_BACKOFF_MAX_ATTEMPT) b->attempt++;
    return delay;
}

bool net_backoff_link_earned_reset(uint32_t up_ms) { return up_ms >= 10000; }

// ── refusals in a row ───────────────────────────────────────────────────────────────────────────────

void net_refusal_reset(net_refusal_t *r) { r->refusals = 0; }

net_event_t net_handshake_event(net_refusal_t *r, bool refused)
{
    if (!refused) {
        r->refusals = 0;
        return NET_EV_TLS_FAIL;
    }
    if (r->refusals < NET_REFUSALS_TO_AUTH_FAILED) r->refusals++;   // saturates: a long run of refusals stays a refusal
    return r->refusals >= NET_REFUSALS_TO_AUTH_FAILED ? NET_EV_TLS_REFUSED : NET_EV_TLS_FAIL;
}

const char *net_state_line(net_state_t state)
{
    switch (state) {
    case NET_OFF:                return "Wi-Fi: off";
    case NET_JOINING:            return "Wi-Fi: joining";
    case NET_NO_IP:              return "Wi-Fi: no address";
    case NET_DAEMON_UNREACHABLE: return "Wi-Fi: daemon not reachable";
    case NET_AUTH_FAILED:        return "Wi-Fi: key refused";
    case NET_UP:                 return "Wi-Fi: connected";
    case NET_STATE_COUNT:        break;
    }
    return "";
}
