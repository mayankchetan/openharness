// link_auto.c (wifi-first work, T1): the decisions of the unplugged mode "Wi-Fi first, else phone", as pure logic. Spec:
// workbench briefs/research/2026-11-11-wifi-first-phone-fallback-spec.md (rev 2). Every number below is the spec's (sections 3.2 to 3.4) and is read
// from link_auto.h through its macros, so a constant that moves moves the test with it; the edges (n - 1 ms and n ms) are what is pinned.
//
//   - the RTC record: trusted only after a software reset with a good checksum and values in range; any other reset ZEROES it and re-seals it
//     (finding 4), so a later software reset reads Wi-Fi;
//   - the Wi-Fi attempt: works / two scans without the access point / a TLS alert seen (sticky) / the 30 s window (findings 2 and 3);
//   - Wi-Fi lost after it worked: 120 s;
//   - the phone boot: drop waits 30, 60, 120 s by retry (0-based, guard BEFORE the index: finding 5), never connected 120 s (Q2);
//   - the bounds: 3 retries, edge cap 8, 600 s of a stable session clears both;
//   - a seeded random simulation over many wakes: at most 7 reboots per wake, and no stretch without a link-attempt trigger beyond 120 s.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_auto.h"

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define CHECK_VOID(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static la_out_t step(la_t *a, uint64_t now, bool up, unsigned noap, bool alert)
{
    la_in_t in = {.now_ms = now, .up = up, .noap_scans = (uint8_t)noap, .alert_seen = alert};
    la_out_t out;
    la_step(a, &in, &out);
    return out;
}

static la_out_t step_h(la_t *a, uint64_t now, bool up, bool hold)
{
    la_in_t in = {.now_ms = now, .up = up, .hold = hold};
    la_out_t out;
    la_step(a, &in, &out);
    return out;
}

static void start(la_t *a, la_target_t running, bool phone_boot, unsigned retries, unsigned edges, uint64_t at)
{
    la_view_t v = {.phone_boot = phone_boot, .retries = (uint8_t)retries, .edges = (uint8_t)edges};
    la_init(a, &v, running, at);
}

// ── the constants ───────────────────────────────────────────────────────────────────────────────────
static int test_constants(void)
{
    CHECK(LA_TRY_MS == 30000u);          // finding 2: 15 s join + 5 s TCP + 5 s TLS = 25 s, plus 20 percent
    CHECK(LA_NOAP_SCANS == 2u);
    CHECK(LA_WIFI_LOST_MS == 120000u);   // Q1
    CHECK(LA_NEVER_MS == 120000u);       // Q2
    CHECK(LA_RETRY_MAX == 3u);
    CHECK(LA_EDGE_CAP == 8u);
    CHECK(LA_STABLE_MS == 600000u);
    CHECK(la_drop_wait_ms(0) == 30000u && la_drop_wait_ms(1) == 60000u && la_drop_wait_ms(2) == 120000u);
    CHECK(la_drop_wait_ms(3) == 0 && la_drop_wait_ms(4) == 0 && la_drop_wait_ms(250) == 0 && la_drop_wait_ms(255) == 0);   // finding 5: no table read
    CHECK(sizeof(la_rec_t) == 12);       // 12 B of RTC memory (spec section 2)
    return 0;
}

// ── the record ──────────────────────────────────────────────────────────────────────────────────────
static int test_record(void)
{
    la_rec_t r;
    la_view_t v;

    // trusted: a software reset, a good checksum, values in range
    la_seal(&r, true, 2, 3);
    memset(&v, 0x55, sizeof v);
    CHECK(la_boot(&r, true, &v));
    CHECK(v.phone_boot && v.retries == 2 && v.edges == 3);
    la_seal(&r, false, 0, 0);
    CHECK(la_boot(&r, true, &v) && !v.phone_boot && v.retries == 0 && v.edges == 0);
    // the cap itself is in range
    la_seal(&r, true, LA_RETRY_MAX, LA_EDGE_CAP);
    CHECK(la_boot(&r, true, &v) && v.phone_boot && v.retries == LA_RETRY_MAX && v.edges == LA_EDGE_CAP);

    // a bad checksum: untrusted, zeros, and the record is zeroed and re-sealed
    la_seal(&r, true, 2, 3);
    r.check ^= 1;
    memset(&v, 0x55, sizeof v);
    CHECK(!la_boot(&r, true, &v));
    CHECK(!v.phone_boot && v.retries == 0 && v.edges == 0);
    CHECK(r.check == la_rec_crc(&r) && r.magic == LA_REC_MAGIC);   // sealed again
    CHECK(la_boot(&r, true, &v) && !v.phone_boot);                  // so the next software reset reads Wi-Fi, never the old phone_boot

    // a bad magic
    la_seal(&r, true, 1, 1);
    r.magic ^= 0x100;
    CHECK(!la_boot(&r, true, &v) && !v.phone_boot);

    // out of range, with a good checksum: phone_boot 7, retries over the cap, edges over the cap
    la_seal(&r, false, 0, 0);
    r.phone_boot = 7;
    r.check = la_rec_crc(&r);
    memset(&v, 0x55, sizeof v);
    CHECK(!la_boot(&r, true, &v) && !v.phone_boot && v.retries == 0);
    la_seal(&r, true, 0, 0);
    r.retries = LA_RETRY_MAX + 1;
    r.check = la_rec_crc(&r);
    CHECK(!la_boot(&r, true, &v) && v.retries == 0);
    la_seal(&r, true, 0, 0);
    r.edges = LA_EDGE_CAP + 1;
    r.check = la_rec_crc(&r);
    CHECK(!la_boot(&r, true, &v) && v.edges == 0);
    la_seal(&r, true, 0, 0);
    r.retries = 250;
    r.check = la_rec_crc(&r);
    CHECK(!la_boot(&r, true, &v));

    // finding 4: a reset that is not a software one (power-on, PWR wake, panic, watchdog) ZEROES the record, even when it is valid. A phone_boot that
    // survived a PWR wake in RTC memory must not be believed by a later software reset.
    la_seal(&r, true, 2, 5);
    memset(&v, 0x55, sizeof v);
    CHECK(!la_boot(&r, false, &v));
    CHECK(!v.phone_boot && v.retries == 0 && v.edges == 0);
    CHECK(la_boot(&r, true, &v));                                   // a later software reset trusts the zeroed record ...
    CHECK(!v.phone_boot && v.retries == 0 && v.edges == 0);         // ... and it says Wi-Fi

    // garbage (a power cut that left whatever was there)
    memset(&r, 0xA5, sizeof r);
    CHECK(!la_boot(&r, true, &v) && !v.phone_boot);
    CHECK(la_boot(&r, true, &v));

    // null safety
    CHECK(!la_boot(NULL, true, &v) && !v.phone_boot && v.retries == 0);
    CHECK(la_boot(&r, true, NULL));    // the view is optional (the record above was sealed by the last untrusted boot)
    la_seal(NULL, true, 1, 1);
    la_rec_crc(NULL);
    return 0;
}

// ── the Wi-Fi attempt ───────────────────────────────────────────────────────────────────────────────
static int test_wifi_attempt(void)
{
    la_t a;
    la_out_t o;

    // works at any time: no edge, and the works event says when
    start(&a, LA_TARGET_WIFI, false, 0, 0, 1000);
    o = step(&a, 1000 + 4000, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_WORKS) && o.waited_ms == 4000);
    o = step(&a, 1000 + 4250, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE && !(o.events & LA_EV_WORKS));   // said once
    o = step(&a, 1000 + LA_TRY_MS + 90000, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE);                                // works: the window rule has stopped for this boot

    // the window: not works by LA_TRY_MS -> the phone. 29,999 and 30,000 ms.
    start(&a, LA_TARGET_WIFI, false, 0, 0, 1000);
    o = step(&a, 1000, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 1000 + LA_TRY_MS - 1, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 1000 + LA_TRY_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_WINDOW && o.waited_ms == LA_TRY_MS);
    CHECK(o.next.phone_boot && o.next.retries == 0 && o.next.edges == 1);   // the retry count is kept, the edge is counted
    // level, not edge: still there at the next step (the glue acts on it only when the mode switch lets the reboot go)
    o = step(&a, 1000 + LA_TRY_MS + 250, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.next.edges == 1);

    // up exactly at the deadline is still "works": the first true row wins
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, LA_TRY_MS, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_WORKS));

    // the window with the retries kept: a retry that fails goes back to the phone with the same count
    start(&a, LA_TARGET_WIFI, false, 2, 5, 0);
    o = step(&a, LA_TRY_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.next.phone_boot && o.next.retries == 2 && o.next.edges == 6);

    // SSID not found: two scans in a row, at once; one scan is not enough
    start(&a, LA_TARGET_WIFI, false, 0, 0, 500);
    o = step(&a, 500 + 3000, false, 1, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 500 + 4000, false, 2, false);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_NO_AP && o.waited_ms == 4000);
    CHECK(la_noap_reason(201) && la_noap_reason(210) && la_noap_reason(211) && la_noap_reason(212));
    CHECK(!la_noap_reason(200) && !la_noap_reason(202) && !la_noap_reason(205) && !la_noap_reason(0) && !la_noap_reason(-1) && !la_noap_reason(213));
    // the scan count wifi_sta.c keeps: one more for each "no AP" disconnect, back to zero for any other reason, held at 255
    CHECK(la_noap_next(0, 201) == 1 && la_noap_next(1, 210) == 2 && la_noap_next(2, 211) == 3 && la_noap_next(3, 212) == 4);
    CHECK(la_noap_next(2, 202) == 0 && la_noap_next(1, 200) == 0 && la_noap_next(1, 4) == 0 && la_noap_next(1, -1) == 0);   // a wrong password, a beacon timeout: not a scan miss
    CHECK(la_noap_next(255, 201) == 255 && la_noap_next(254, 201) == 255);
    // more scans than the limit still fire (a counter that jumped)
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, 5000, false, 9, false);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_NO_AP);
    // two scans, but the link works: nothing (a stale count after the join)
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, 5000, true, 2, false);
    CHECK(o.target == LA_TARGET_NONE);

    // finding 3: a TLS alert seen is sticky and holds Wi-Fi: no window edge, no scan edge, even with the link not up (the state is
    // DAEMON_UNREACHABLE between two refusals, which is why the flag and not the state is the test)
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, 3000, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 5000, false, 0, true);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_HOLD_ALERT));
    o = step(&a, 5250, false, 0, true);
    CHECK(!(o.events & LA_EV_HOLD_ALERT));                              // once per episode
    o = step(&a, LA_TRY_MS, false, 0, false);                           // the input flag fell; the latch did not
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, LA_TRY_MS + 1, false, 0, true);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 600000, false, 0, true);
    CHECK(o.target == LA_TARGET_NONE);
    // review M1: the access point goes away after an alert (out of range): the hold means nothing without a daemon to reach, so two scans in a row win
    o = step(&a, 600250, false, 1, true);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 600500, false, 2, true);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_NO_AP);
    // an alert seen in the very step of the deadline holds it
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, LA_TRY_MS, false, 0, true);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_HOLD_ALERT));

    // an alert, then the link works: ordinary working Wi-Fi
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    o = step(&a, 4000, false, 0, true);
    o = step(&a, 9000, true, 0, true);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_WORKS));

    // Wi-Fi lost after it worked (Q1): 120 s, then the phone; not a retry
    start(&a, LA_TARGET_WIFI, false, 1, 2, 0);
    o = step(&a, 5000, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 100000, false, 0, false);                              // lost here
    CHECK(o.target == LA_TARGET_NONE);                                  // the window rule is over: 30 s after the start is long past
    o = step(&a, 100000 + LA_WIFI_LOST_MS - 1, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 100000 + LA_WIFI_LOST_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_WIFI_LOST && o.waited_ms == LA_WIFI_LOST_MS);
    CHECK(o.next.phone_boot && o.next.retries == 1 && o.next.edges == 3);   // retries unchanged, edge counted
    // a loss that heals inside the 120 s restarts the clock
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 50000, false, 0, false);
    o = step(&a, 50000 + 119000, true, 0, false);                       // back after 119 s
    CHECK(o.target == LA_TARGET_NONE);
    step(&a, 200000, false, 0, false);                                  // lost again
    o = step(&a, 200000 + LA_WIFI_LOST_MS - 1, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 200000 + LA_WIFI_LOST_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_WIFI_LOST);
    // lost soon after "works", scans and alerts after that are not the rules of the lost state
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    step(&a, 2000, true, 0, false);
    o = step(&a, 3000, false, 5, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, LA_TRY_MS + 5000, false, 5, false);
    CHECK(o.target == LA_TARGET_NONE);

    // review H1: the S2 radio park (wifi off after 5 min dark) is not a lost link. `hold` is net_link_radio_waking(): true while the radio is parked and
    // while it wakes, until the link is up again or 30 s have passed.
    {
        // dark for 7 minutes: the link is down because the radio is off. No edge, and no clock.
        start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
        step_h(&a, 5000, true, false);                                  // Wi-Fi works
        o = step_h(&a, 300000, false, true);                            // parked at 5 min
        CHECK(o.target == LA_TARGET_NONE);
        for (uint64_t t = 300250; t <= 300000 + 7 * 60000; t += 250) {
            o = step_h(&a, t, false, true);
            CHECK(o.target == LA_TARGET_NONE);
        }
        // park -> wake -> join ok: the hold ends with the link up, and nothing ever fires
        o = step_h(&a, 800000, false, true);                            // waking
        o = step_h(&a, 806000, true, false);                            // the session is up again
        CHECK(o.target == LA_TARGET_NONE);
        o = step_h(&a, 806000 + LA_WIFI_LOST_MS * 2, true, false);
        CHECK(o.target == LA_TARGET_NONE);
        // park -> wake -> the reconnect fails: the 120 s start when the hold ends (30 s after the wake), not before
        start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
        step_h(&a, 5000, true, false);
        step_h(&a, 300000, false, true);
        step_h(&a, 700000, false, true);                                // the wake
        step_h(&a, 700000 + 29750, false, true);
        o = step_h(&a, 730000, false, false);                           // 30 s of waking are over, still no link: the lost clock starts here
        CHECK(o.target == LA_TARGET_NONE);
        o = step_h(&a, 730000 + LA_WIFI_LOST_MS - 1, false, false);
        CHECK(o.target == LA_TARGET_NONE);
        o = step_h(&a, 730000 + LA_WIFI_LOST_MS, false, false);
        CHECK(o.target == LA_TARGET_PHONE && o.why == LA_WHY_WIFI_LOST && o.next.retries == 0);
        // a hold that comes back inside the 120 s freezes it again and restarts the count
        start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
        step_h(&a, 1000, true, false);
        step_h(&a, 10000, false, false);
        o = step_h(&a, 10000 + 100000, false, true);
        CHECK(o.target == LA_TARGET_NONE);
        o = step_h(&a, 10000 + 100000 + 119999, false, false);
        CHECK(o.target == LA_TARGET_NONE);
    }

    // the edge cap: a Wi-Fi attempt that failed at 8 edges does not fire
    start(&a, LA_TARGET_WIFI, false, 0, LA_EDGE_CAP, 0);
    o = step(&a, LA_TRY_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_REFUSE_EDGES));
    o = step(&a, LA_TRY_MS + 250, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE && !(o.events & LA_EV_REFUSE_EDGES));   // once
    start(&a, LA_TARGET_WIFI, false, 0, LA_EDGE_CAP - 1, 0);
    o = step(&a, LA_TRY_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.next.edges == LA_EDGE_CAP);
    return 0;
}

// ── the phone boot ──────────────────────────────────────────────────────────────────────────────────
static int test_phone_drop(void)
{
    la_t a;
    la_out_t o;
    // finding 5 and spec 3.4: waits by 0-based retries
    for (unsigned r = 0; r < LA_RETRY_MAX; r++) {
        const uint32_t wait = la_drop_wait_ms((uint8_t)r);
        CHECK(wait == (r == 0 ? 30000u : r == 1 ? 60000u : 120000u));
        start(&a, LA_TARGET_PHONE, true, r, r + 1, 7000);
        o = step(&a, 7000 + 3000, true, 0, false);                       // up, then down
        CHECK(o.target == LA_TARGET_NONE);
        o = step(&a, 20000, false, 0, false);                            // down at 20 s
        CHECK(o.target == LA_TARGET_NONE);
        o = step(&a, 20000 + wait - 1, false, 0, false);
        CHECK(o.target == LA_TARGET_NONE);
        o = step(&a, 20000 + wait, false, 0, false);
        CHECK(o.target == LA_TARGET_WIFI && o.why == LA_WHY_DROP && o.waited_ms == wait);
        CHECK(!o.next.phone_boot && o.next.retries == r + 1 && o.next.edges == r + 2);   // 0-based count used so far, +1
    }

    // the guard comes BEFORE the table: 3 and 250 (and 255) are refused, and the table is never read (-fsanitize=bounds would stop the run)
    const unsigned bad[] = {LA_RETRY_MAX, LA_RETRY_MAX + 1, 250, 255};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        start(&a, LA_TARGET_PHONE, true, bad[i], 0, 0);
        step(&a, 1000, true, 0, false);
        o = step(&a, 2000, false, 0, false);                             // the drop: refused at once, said once
        CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_REFUSE_RETRIES));
        o = step(&a, 2000 + 10000000, false, 0, false);
        CHECK(o.target == LA_TARGET_NONE && !(o.events & LA_EV_REFUSE_RETRIES));
        o = step(&a, 2000 + 10000250, false, 0, false);
        CHECK(!(o.events & LA_EV_REFUSE_RETRIES));                       // once per episode
    }

    // the session comes back inside the wait: the timer restarts at the next drop
    start(&a, LA_TARGET_PHONE, true, 0, 0, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 5000, false, 0, false);                                    // down
    o = step(&a, 5000 + 29000, true, 0, false);                         // back after 29 s
    CHECK(o.target == LA_TARGET_NONE);
    step(&a, 40000, false, 0, false);                                   // down again at 40 s
    o = step(&a, 40000 + 29999, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);                                  // the first drop's clock is gone
    o = step(&a, 40000 + 30000, false, 0, false);
    CHECK(o.target == LA_TARGET_WIFI);

    // a drop that has already fired still fires when the busy gate lets it through later (level, not edge)
    o = step(&a, 40000 + 30000 + 50000, false, 0, false);
    CHECK(o.target == LA_TARGET_WIFI && o.next.retries == 1);

    // a clock that steps back neither fires nor rewinds
    start(&a, LA_TARGET_PHONE, true, 0, 0, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 10000, false, 0, false);
    o = step(&a, 5000, false, 0, false);                                // back by 5 s
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 10000 + 29999, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 10000 + 30000, false, 0, false);
    CHECK(o.target == LA_TARGET_WIFI);

    // the edge cap holds the phone's retry as well
    start(&a, LA_TARGET_PHONE, true, 0, LA_EDGE_CAP, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 2000, false, 0, false);
    o = step(&a, 2000 + 30000, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_REFUSE_EDGES));
    start(&a, LA_TARGET_PHONE, true, 0, LA_EDGE_CAP - 1, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 2000, false, 0, false);
    o = step(&a, 2000 + 30000, false, 0, false);
    CHECK(o.target == LA_TARGET_WIFI && o.next.edges == LA_EDGE_CAP);
    return 0;
}

static int test_phone_never_connected(void)
{
    la_t a;
    la_out_t o;
    // Q2: 120 s from the BLE start, the same for every retry
    for (unsigned r = 0; r < LA_RETRY_MAX; r++) {
        start(&a, LA_TARGET_PHONE, true, r, r + 1, 9000);
        o = step(&a, 9000, false, 0, false);
        CHECK(o.target == LA_TARGET_NONE);
        o = step(&a, 9000 + LA_NEVER_MS - 1, false, 0, false);
        CHECK(o.target == LA_TARGET_NONE);
        o = step(&a, 9000 + LA_NEVER_MS, false, 0, false);
        CHECK(o.target == LA_TARGET_WIFI && o.why == LA_WHY_NEVER && o.waited_ms == LA_NEVER_MS);
        CHECK(!o.next.phone_boot && o.next.retries == r + 1 && o.next.edges == r + 2);
    }
    // the 4th trigger does nothing, and the reason is said once
    start(&a, LA_TARGET_PHONE, true, LA_RETRY_MAX, 6, 0);
    o = step(&a, LA_NEVER_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE && (o.events & LA_EV_REFUSE_RETRIES));
    o = step(&a, LA_NEVER_MS + 5000, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE && !(o.events & LA_EV_REFUSE_RETRIES));
    // retries 250 never indexes anything either
    start(&a, LA_TARGET_PHONE, true, 250, 0, 0);
    o = step(&a, LA_NEVER_MS * 10, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    // connected once: the never-connected rule is gone for this boot (the drop rule takes over)
    start(&a, LA_TARGET_PHONE, true, 0, 0, 0);
    o = step(&a, 100000, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    o = step(&a, 119999 + 50000, true, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    // the phone boot does not read the Wi-Fi inputs: scans and an alert mean nothing to it
    start(&a, LA_TARGET_PHONE, true, 0, 0, 0);
    o = step(&a, 1000, false, 9, true);
    CHECK(o.target == LA_TARGET_NONE && !(o.events & LA_EV_HOLD_ALERT));
    o = step(&a, LA_NEVER_MS, false, 9, true);
    CHECK(o.target == LA_TARGET_WIFI && o.why == LA_WHY_NEVER);
    return 0;
}

static int test_stable_and_inactive(void)
{
    la_t a;
    la_out_t o;
    // 600 s of a stable session clear retries and edges (either mode)
    start(&a, LA_TARGET_PHONE, true, 2, 6, 0);
    o = step(&a, 1000, true, 0, false);
    CHECK(!(o.events & LA_EV_STABLE));
    o = step(&a, 1000 + LA_STABLE_MS - 1, true, 0, false);
    CHECK(!(o.events & LA_EV_STABLE));
    o = step(&a, 1000 + LA_STABLE_MS, true, 0, false);
    CHECK((o.events & LA_EV_STABLE) && o.now.phone_boot && o.now.retries == 0 && o.now.edges == 0);
    o = step(&a, 1000 + LA_STABLE_MS + 250, true, 0, false);
    CHECK(!(o.events & LA_EV_STABLE));                                  // once
    // after the clear the drop is retry 0 again: 30 s
    step(&a, 700000, false, 0, false);
    o = step(&a, 700000 + 30000, false, 0, false);
    CHECK(o.target == LA_TARGET_WIFI && o.next.retries == 1 && o.next.edges == 1);
    // a drop inside the 600 s restarts the stable clock
    start(&a, LA_TARGET_WIFI, false, 1, 3, 0);
    step(&a, 1000, true, 0, false);
    step(&a, 500000, false, 0, false);
    step(&a, 510000, true, 0, false);
    o = step(&a, 1000 + LA_STABLE_MS + 9000, true, 0, false);
    CHECK(!(o.events & LA_EV_STABLE));
    o = step(&a, 510000 + LA_STABLE_MS, true, 0, false);
    CHECK((o.events & LA_EV_STABLE) && o.now.retries == 0 && o.now.edges == 0 && !o.now.phone_boot);
    // nothing to clear: no event
    start(&a, LA_TARGET_WIFI, false, 0, 0, 0);
    step(&a, 1000, true, 0, false);
    o = step(&a, 1000 + LA_STABLE_MS, true, 0, false);
    CHECK(!(o.events & LA_EV_STABLE));
    // the cleared count lifts the edge cap
    start(&a, LA_TARGET_WIFI, false, 3, LA_EDGE_CAP, 0);
    step(&a, 1000, true, 0, false);
    o = step(&a, 1000 + LA_STABLE_MS, true, 0, false);
    CHECK(o.events & LA_EV_STABLE);
    step(&a, 700000, false, 0, false);
    o = step(&a, 700000 + LA_WIFI_LOST_MS, false, 0, false);
    CHECK(o.target == LA_TARGET_PHONE && o.next.edges == 1);

    // not running a wireless mode (USB): nothing, whatever the inputs say
    start(&a, LA_TARGET_NONE, false, 0, 0, 0);
    o = step(&a, LA_TRY_MS * 10, false, 9, true);
    CHECK(o.target == LA_TARGET_NONE && o.events == 0);
    // null safety
    la_init(NULL, NULL, LA_TARGET_WIFI, 0);
    la_step(NULL, NULL, NULL);
    la_step(&a, NULL, &o);
    CHECK(o.target == LA_TARGET_NONE);
    // an out-of-range target is not a mode
    start(&a, (la_target_t)9, false, 0, 0, 0);
    o = step(&a, LA_TRY_MS * 2, false, 0, false);
    CHECK(o.target == LA_TARGET_NONE);
    CHECK(!strcmp(la_target_name(LA_TARGET_WIFI), "wifi") && !strcmp(la_target_name(LA_TARGET_PHONE), "phone") && !strcmp(la_target_name(LA_TARGET_NONE), "none"));
    CHECK(!strcmp(la_target_name((la_target_t)9), "?"));
    for (int w = 0; w <= LA_WHY_COUNT; w++) CHECK(la_why_name((la_why_t)w)[0] != 0);
    return 0;
}

// ── the seeded simulation ───────────────────────────────────────────────────────────────────────────
static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;   // the classic LCG, enough for a seeded property run
    return rng_state >> 8;
}
static uint32_t rnd_in(uint32_t lo, uint32_t hi) { return lo + rnd() % (hi - lo + 1); }

#define SIM_BOOT_MS 400000u   // one boot is simulated this long; every up run below is shorter than LA_STABLE_MS, so the 600 s clear never fires
#define SIM_STEP_MS 250u
#define MAX_RUNS 3

typedef struct {
    uint32_t from[MAX_RUNS], to[MAX_RUNS];
    int n;
    bool ap_absent;       // Wi-Fi boots: the scans find nothing
    bool alert;           // Wi-Fi boots: the daemon answers a handshake with an alert (from 4 s)
} world_t;

static void make_world(world_t *w, bool phone)
{
    memset(w, 0, sizeof *w);
    const uint32_t kind = rnd() % 10;
    w->n = kind < 3 ? 0 : kind < 7 ? 1 : kind < 9 ? 2 : 3;
    uint32_t t = rnd_in(500, phone ? 250000 : 40000);
    for (int i = 0; i < w->n; i++) {
        w->from[i] = t;
        w->to[i] = t + rnd_in(1000, 300000);
        t = w->to[i] + rnd_in(500, 150000);
    }
    w->ap_absent = !phone && w->n == 0 && rnd() % 2;
    w->alert = !phone && rnd() % 20 == 0;
    // a world must not hold a run that reaches the stable limit: keep every run under LA_STABLE_MS
    for (int i = 0; i < w->n; i++)
        if (w->to[i] - w->from[i] >= LA_STABLE_MS) w->to[i] = w->from[i] + LA_STABLE_MS - 1;
}

static bool up_at(const world_t *w, uint32_t t)
{
    for (int i = 0; i < w->n; i++)
        if (t >= w->from[i] && t < w->to[i]) return true;
    return false;
}

static int test_simulation(void)
{
    unsigned max_reboots = 0, total_wakes = 0, total_reboots = 0, held = 0, stuck = 0;
    uint32_t worst_down = 0;
    rng_state = 0x5EEDC0DEu;   // seeded: the run is the same every time
    for (unsigned wake = 0; wake < 4000; wake++) {
        la_rec_t rec;
        memset(&rec, 0xA5, sizeof rec);                                  // a power-on or PWR wake: whatever the RTC kept
        la_view_t v;
        CHECK_VOID(!la_boot(&rec, false, &v));
        unsigned reboots = 0;
        for (;;) {
            CHECK_VOID(la_boot(&rec, true, &v) || reboots == 0);         // after the first boot every reset is a software one with a good record
            const bool phone = v.phone_boot;
            if (reboots == 0) CHECK_VOID(!phone);                        // never phone first
            la_t a;
            la_init(&a, &v, phone ? LA_TARGET_PHONE : LA_TARGET_WIFI, 0);
            world_t w;
            make_world(&w, phone);
            uint32_t down_since = 0;
            bool prev_up = false, rebooted = false;
            for (uint32_t t = 0; t < SIM_BOOT_MS; t += SIM_STEP_MS) {
                const bool up = up_at(&w, t);
                if (up || prev_up) down_since = t;                       // a no-link stretch starts at the first step without a link
                prev_up = up;
                const unsigned noap = (!phone && w.ap_absent && t >= 2000) ? (unsigned)(t / 3000) : 0;
                const bool alert = w.alert && t >= 4000;
                la_in_t in = {.now_ms = t, .up = up, .noap_scans = (uint8_t)(noap > 255 ? 255 : noap), .alert_seen = alert};
                la_out_t o;
                la_step(&a, &in, &o);
                if (o.events & LA_EV_HOLD_ALERT) held++;
                if (o.target != LA_TARGET_NONE) {
                    CHECK_VOID(o.target == (phone ? LA_TARGET_WIFI : LA_TARGET_PHONE));
                    CHECK_VOID(o.next.phone_boot == !phone);
                    CHECK_VOID(o.next.edges == v.edges + 1 && o.next.edges <= LA_EDGE_CAP);
                    CHECK_VOID(o.next.retries == (phone ? v.retries + 1u : v.retries) && o.next.retries <= LA_RETRY_MAX);
                    la_seal(&rec, o.next.phone_boot, o.next.retries, o.next.edges);   // seal, then "restart"
                    reboots++;
                    rebooted = true;
                    break;
                }
                // the bound: while an edge could fire (no alert hold, retries and edges not spent), no stretch of no link goes past 120 s
                const bool can_fire = !alert && v.edges < LA_EDGE_CAP && (!phone || v.retries < LA_RETRY_MAX);
                if (!up && can_fire) {
                    const uint32_t down = t - down_since;
                    if (down > worst_down) worst_down = down;
                    CHECK_VOID(down <= LA_NEVER_MS + SIM_STEP_MS);        // 120 s is the longest of every trigger (never connected, third drop, Wi-Fi lost)
                }
            }
            if (!rebooted) {
                stuck++;   // the boot settled: a link that works, or a spent budget / a held alert
                break;
            }
            CHECK_VOID(reboots <= 1 + 2 * LA_RETRY_MAX);                 // 1 + 2 x 3 = 7 per wake
        }
        if (reboots > max_reboots) max_reboots = reboots;
        total_reboots += reboots;
        total_wakes++;
    }
    CHECK(max_reboots <= 7);
    CHECK(max_reboots >= 3);        // the run does exercise the retries
    CHECK(held > 0 && stuck == total_wakes);
    printf("  simulation: %u wakes, %u reboots, max %u per wake, longest no-link stretch %u ms, alert holds %u\n", total_wakes, total_reboots,
           max_reboots, (unsigned)worst_down, held);
    return 0;
}

int main(void)
{
    if (test_constants() || test_record() || test_wifi_attempt() || test_phone_drop() || test_phone_never_connected() || test_stable_and_inactive() ||
        test_simulation())
        return 1;
    printf("test_link_auto: %u checks PASS\n", checks);
    return 0;
}
