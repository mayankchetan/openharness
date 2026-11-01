// W11: the Wi-Fi mode's link state machine and reconnect timing (net_state.c): the whole transition table, the captions, the backoff
// ladders and the jitter bounds. Pure logic: no IDF, no clock, no RNG (the caller passes the random word).
//
//   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=undefined,bounds -I ../main test_net_state.c ../main/net_state.c
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_state.h"

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

// The table, written out a second time from the spec text (docs: rows are states, columns are events), as letters:
//   O off, J joining, N no IP, D daemon unreachable, A auth failed, U up.
// Event order: START STOP WIFI_DOWN GOT_IP LOST_IP IP_TIMEOUT TCP_FAIL TLS_FAIL TLS_REFUSED LINK_UP LINK_DOWN
static const char *const EXPECT[NET_STATE_COUNT] = {
    /* OFF  */ "JOOOOOOOOOO",
    /* JOIN */ "JOJJJNDDAUJ",
    /* NOIP */ "NOJJNNNNNNN",
    /* DAEM */ "DOJDNDDDAUD",
    /* AUTH */ "AOJANADDAUA",
    /* UP   */ "UOJUNUDDAUJ",
};

static net_state_t letter(char c)
{
    switch (c) {
    case 'O': return NET_OFF;
    case 'J': return NET_JOINING;
    case 'N': return NET_NO_IP;
    case 'D': return NET_DAEMON_UNREACHABLE;
    case 'A': return NET_AUTH_FAILED;
    case 'U': return NET_UP;
    }
    assert(!"bad letter");
    return NET_OFF;
}

static void test_table(void)
{
    CHECK(NET_STATE_COUNT == 6 && NET_EV_COUNT == 11);
    for (int s = 0; s < NET_STATE_COUNT; s++) {
        CHECK(strlen(EXPECT[s]) == NET_EV_COUNT);
        for (int e = 0; e < NET_EV_COUNT; e++) {
            const net_state_t got = net_state_next((net_state_t)s, (net_event_t)e);
            if (got != letter(EXPECT[s][e])) {
                printf("table: state %s event %d: got %s, expected %c\n", net_state_name((net_state_t)s), e, net_state_name(got), EXPECT[s][e]);
                assert(0);
            }
            checks++;
        }
    }
    // Out-of-range input leaves the state alone and never reads outside the table.
    CHECK(net_state_next((net_state_t)99, NET_EV_START) == NET_OFF);
    CHECK(net_state_next(NET_UP, (net_event_t)99) == NET_UP);
    // STOP from every state is OFF, and OFF takes nothing but START.
    for (int s = 0; s < NET_STATE_COUNT; s++) CHECK(net_state_next((net_state_t)s, NET_EV_STOP) == NET_OFF);
    for (int e = 0; e < NET_EV_COUNT; e++)
        CHECK(net_state_next(NET_OFF, (net_event_t)e) == (e == NET_EV_START ? NET_JOINING : NET_OFF));
    // The only way into UP is a finished handshake.
    for (int s = 0; s < NET_STATE_COUNT; s++)
        for (int e = 0; e < NET_EV_COUNT; e++)
            if (net_state_next((net_state_t)s, (net_event_t)e) == NET_UP && s != NET_UP) CHECK(e == NET_EV_LINK_UP);
}

static void test_names_and_captions(void)
{
    static const char *const names[] = { "off", "joining", "no_ip", "daemon_unreachable", "auth_failed", "up" };
    for (int s = 0; s < NET_STATE_COUNT; s++) CHECK(!strcmp(net_state_name((net_state_t)s), names[s]));
    CHECK(!strcmp(net_state_name((net_state_t)99), "?"));
    // Nothing to say when off or up; a short line (it sits under the wordmark) for every other state, all different.
    CHECK(net_state_caption(NET_OFF) == NULL && net_state_caption(NET_UP) == NULL);
    const char *seen[4];
    int n = 0;
    for (int s = NET_JOINING; s <= NET_AUTH_FAILED; s++) {
        const char *c = net_state_caption((net_state_t)s);
        CHECK(c && *c && strlen(c) <= 24);
        for (int i = 0; i < n; i++) CHECK(strcmp(seen[i], c) != 0);
        seen[n++] = c;
    }
    CHECK(n == 4);
    // The two states a person can act on say what to do about it: the cable fixes the second.
    CHECK(!strcmp(net_state_caption(NET_AUTH_FAILED), "Key refused: reset dial"));   // the USB recovery path comes back with W14
}

// The one line of the Link screen (W16): a line for EVERY state, OFF and UP included, with the names the tests and the logs use
// (OFF, JOINING, NO_IP, DAEMON_UNREACHABLE, AUTH_FAILED, UP). No SSID, password or key can reach it: it takes the state and nothing else.
static void test_link_screen_line(void)
{
    static const char *const lines[] = { "Wi-Fi: off", "Wi-Fi: joining", "Wi-Fi: no address", "Wi-Fi: daemon not reachable", "Wi-Fi: key refused", "Wi-Fi: connected" };
    for (int s = 0; s < NET_STATE_COUNT; s++) {
        const char *l = net_state_line((net_state_t)s);
        CHECK(l && !strcmp(l, lines[s]));
        CHECK(strlen(l) <= 28);   // 466 px of Inter 20 is more than 28 characters; the mono skin's 17-cell row would wrap, so it is wrapped in the UI
    }
    CHECK(net_state_line((net_state_t)99) && !strcmp(net_state_line((net_state_t)99), ""));
}

static void test_ladder(void)
{
    static const uint32_t normal[] = { 1000, 2000, 4000, 8000, 16000, 30000, 60000 };
    static const uint32_t refused[] = { 30000, 60000, 120000, 300000 };
    for (unsigned i = 0; i < 7; i++) CHECK(net_backoff_base_ms(i, false) == normal[i]);
    for (unsigned i = 0; i < 4; i++) CHECK(net_backoff_base_ms(i, true) == refused[i]);
    // Past the end the last step repeats, for any attempt count.
    CHECK(net_backoff_base_ms(7, false) == 60000 && net_backoff_base_ms(1000, false) == 60000 && net_backoff_base_ms(UINT32_MAX, false) == 60000);
    CHECK(net_backoff_base_ms(4, true) == 300000 && net_backoff_base_ms(UINT32_MAX, true) == 300000);
    // A refusal always waits longer than the same attempt of the ordinary ladder.
    for (unsigned i = 0; i < 12; i++) CHECK(net_backoff_base_ms(i, true) > net_backoff_base_ms(i, false));
}

static void test_jitter(void)
{
    // The delay is in [3/4 base, 5/4 base] for every random word, with both ends reachable, and it never leaves the 32-bit range.
    for (int refusing = 0; refusing < 2; refusing++) {
        for (unsigned attempt = 0; attempt < 10; attempt++) {
            const uint32_t base = net_backoff_base_ms(attempt, refusing);
            uint32_t lo = UINT32_MAX, hi = 0;
            uint32_t r = 0x9E3779B9u;
            for (int i = 0; i < 20000; i++) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                const uint32_t d = net_backoff_jitter_ms(base, r);
                CHECK(d >= base - base / 4 && d <= base + base / 4);
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
            CHECK(lo <= base - base / 4 + base / 100 + 1 && hi >= base + base / 4 - base / 100 - 1);   // within 1 percent of both ends
            CHECK(net_backoff_jitter_ms(base, 0) == base - base / 4);
            CHECK(net_backoff_jitter_ms(base, UINT32_MAX) >= base - base / 4 && net_backoff_jitter_ms(base, UINT32_MAX) <= base + base / 4);
        }
    }
    CHECK(net_backoff_jitter_ms(0, 12345) == 0);
}

static void test_sequence(void)
{
    net_backoff_t b;
    net_backoff_reset(&b);
    // Each call returns the delay for the current attempt and advances; the zero word gives the bottom of each band.
    static const uint32_t bottom[] = { 750, 1500, 3000, 6000, 12000, 22500, 45000, 45000, 45000 };
    for (unsigned i = 0; i < 9; i++) CHECK(net_backoff_next_ms(&b, false, 0) == bottom[i]);
    net_backoff_reset(&b);
    CHECK(net_backoff_next_ms(&b, false, 0) == 750);
    // A refusal restarts nothing: it reads the longer ladder at the same attempt count.
    CHECK(net_backoff_next_ms(&b, true, 0) == 45000);   // attempt 1 of the refused ladder: 60 s, bottom of the band
    // The counter saturates instead of wrapping.
    for (int i = 0; i < 1000; i++) (void)net_backoff_next_ms(&b, false, 0);
    CHECK(b.attempt == NET_BACKOFF_MAX_ATTEMPT);
    CHECK(net_backoff_next_ms(&b, false, 0) == 45000);
    // A link that held for 10 s or more earns a fresh ladder; a flapping one does not.
    CHECK(!net_backoff_link_earned_reset(0) && !net_backoff_link_earned_reset(9999) && net_backoff_link_earned_reset(10000)
          && net_backoff_link_earned_reset(3600000));
}

static void test_refusal_count(void)
{
    // One refusal is not a verdict: a daemon restart, a noisy handshake or a reset all look alike on the first try. Three in a row are.
    net_refusal_t r;
    net_refusal_reset(&r);
    CHECK(NET_REFUSALS_TO_AUTH_FAILED == 3);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_REFUSED);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_REFUSED);   // and it stays promoted while the daemon keeps saying no
    // Any other outcome breaks the run: a failure that is not an alert, or a reset by the caller (a link came up, TCP failed).
    CHECK(net_handshake_event(&r, false) == NET_EV_TLS_FAIL);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL && net_handshake_event(&r, true) == NET_EV_TLS_FAIL);
    net_refusal_reset(&r);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL && net_handshake_event(&r, true) == NET_EV_TLS_FAIL);
    net_refusal_reset(&r);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL);
    CHECK(net_handshake_event(&r, false) == NET_EV_TLS_FAIL);
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_FAIL && net_handshake_event(&r, true) == NET_EV_TLS_FAIL);   // 1, break, 2: not three in a row
    for (int i = 0; i < 1000; i++) (void)net_handshake_event(&r, true);   // the counter saturates instead of wrapping to "not refused"
    CHECK(net_handshake_event(&r, true) == NET_EV_TLS_REFUSED);
    // The machine: three refusals take JOINING to AUTH_FAILED; the first two leave it on the ordinary path (DAEMON_UNREACHABLE, 60 s cap).
    net_refusal_reset(&r);
    net_state_t s = NET_JOINING;
    s = net_state_next(s, net_handshake_event(&r, true));
    CHECK(s == NET_DAEMON_UNREACHABLE);
    s = net_state_next(s, net_handshake_event(&r, true));
    CHECK(s == NET_DAEMON_UNREACHABLE);
    s = net_state_next(s, net_handshake_event(&r, true));
    CHECK(s == NET_AUTH_FAILED);
    // A handshake that fails without an alert afterwards drops back to the ordinary ladder; a good one clears everything.
    s = net_state_next(s, net_handshake_event(&r, false));
    CHECK(s == NET_DAEMON_UNREACHABLE);
    CHECK(net_backoff_base_ms(6, false) == 60000 && net_backoff_base_ms(50, false) == 60000);   // the ordinary ladder never passes 60 s
}

int main(void)
{
    test_refusal_count();
    test_table();
    test_names_and_captions();
    test_link_screen_line();
    test_ladder();
    test_jitter();
    test_sequence();
    printf("net_state: PASS (%u checks; %d x %d transition table, captions, normal and refused ladders, jitter bounds)\n", checks, NET_STATE_COUNT, NET_EV_COUNT);
    return 0;
}
