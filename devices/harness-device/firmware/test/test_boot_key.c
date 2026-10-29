// boot_key.c: the BOOT key state machine (tap, hold-to-talk, the D1 gate, the cold-boot settings-wipe countdown). Pure logic with an
// injected clock, no ESP-IDF.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "boot_key.h"

static int talk_calls;
static bool talk_answer;
static bool talk_ok(void) { talk_calls++; return talk_answer; }

// One poll every 100 ms, as the ptt task runs on external power. Returns the events of the whole run as a string of letters:
// P press, T tap, S hold start, E hold end.
typedef struct { bk_t k; uint32_t now; char log[32]; int n; } rig_t;
static void rig_init(rig_t *r, uint32_t start, bool pressed_at_start)
{
    memset(r, 0, sizeof *r);
    r->now = start;
    bk_init(&r->k, pressed_at_start);
}
static bk_event_t poll(rig_t *r, bool pressed, bool allowed)
{
    r->now += 100;
    const bk_event_t e = bk_step(&r->k, r->now, pressed, allowed, talk_ok);
    if (e != BK_NONE) r->log[r->n++] = "-PTSE"[e];
    return e;
}
// Press for `ms` (a whole number of polls), then release (one poll).
static void press_for(rig_t *r, uint32_t ms, bool allowed)
{
    for (uint32_t t = 0; t < ms; t += 100) poll(r, true, allowed);
    poll(r, false, allowed);
}

static void test_tap(void)
{
    rig_t r; rig_init(&r, 1000, false);
    talk_answer = true; talk_calls = 0;
    for (int i = 0; i < 5; i++) assert(poll(&r, false, true) == BK_NONE);       // idle
    assert(poll(&r, true, true) == BK_PRESS);                                    // the press edge: the caller wakes a dark screen (USB)
    assert(poll(&r, false, true) == BK_TAP);                                     // released at 100 ms: a tap, on the release
    assert(strcmp(r.log, "PT") == 0 && talk_calls == 0);                         // the talk question is never asked for a tap
    // 300 ms is still a tap (the threshold is 400)
    rig_init(&r, 1000, false);
    press_for(&r, 300, true);
    assert(strcmp(r.log, "PT") == 0);
}

static void test_hold(void)
{
    rig_t r; rig_init(&r, 0, false);
    talk_answer = true; talk_calls = 0;
    assert(poll(&r, true, true) == BK_PRESS);            // t=100
    assert(poll(&r, true, true) == BK_NONE);             // 200
    assert(poll(&r, true, true) == BK_NONE);             // 300
    assert(poll(&r, true, true) == BK_NONE);             // 400: 300 ms down
    assert(poll(&r, true, true) == BK_HOLD_START);       // 500: 400 ms down, the hold threshold (BK_HOLD_MS)
    assert(talk_calls == 1);
    for (int i = 0; i < 20; i++) assert(poll(&r, true, true) == BK_NONE);   // held on: no repeat, no more questions
    assert(talk_calls == 1);
    assert(poll(&r, false, true) == BK_HOLD_END);        // release sends
    assert(strcmp(r.log, "PSE") == 0);
    assert(poll(&r, false, true) == BK_NONE);
    // the boundary: released at 399 ms is a tap, at 400 ms the hold has started
    rig_init(&r, 0, false);
    r.now = 0; assert(bk_step(&r.k, 1, true, true, talk_ok) == BK_PRESS);
    assert(bk_step(&r.k, 400, true, true, talk_ok) == BK_NONE);              // 399 ms down
    assert(bk_step(&r.k, 400, false, true, talk_ok) == BK_TAP);
    rig_init(&r, 0, false);
    assert(bk_step(&r.k, 1, true, true, talk_ok) == BK_PRESS);
    assert(bk_step(&r.k, 401, true, true, talk_ok) == BK_HOLD_START);        // 400 ms down
    assert(bk_step(&r.k, 402, false, true, talk_ok) == BK_HOLD_END);
    // two holds in a row
    rig_init(&r, 0, false);
    press_for(&r, 1000, true); press_for(&r, 1000, true);
    assert(strcmp(r.log, "PSEPSE") == 0);
}

static void test_talk_denied(void)
{
    // A turn is running, the link is down, or the view is not a surface: the hold does not start a recording, and the release is
    // today's tap ("interrupt the running turn"). The question is asked once per press.
    rig_t r; rig_init(&r, 0, false);
    talk_answer = false; talk_calls = 0;
    press_for(&r, 3000, true);
    assert(strcmp(r.log, "PT") == 0 && talk_calls == 1);
    talk_answer = true;
    press_for(&r, 1000, true);
    assert(strcmp(r.log, "PTPSE") == 0 && talk_calls == 2);
}

static void test_d1_gate(void)
{
    // On the battery with the screen off BOOT does nothing: no press event (no wake), no tap, no hold, no question. A press made in
    // that state stays ignored to its release even if the screen comes on (PWR) meanwhile.
    rig_t r; rig_init(&r, 0, false);
    talk_answer = true; talk_calls = 0;
    press_for(&r, 3000, false);
    assert(r.n == 0 && talk_calls == 0);
    for (int i = 0; i < 4; i++) poll(&r, true, false);
    for (int i = 0; i < 10; i++) assert(poll(&r, true, true) == BK_NONE);   // allowed again mid-press: still ignored
    assert(poll(&r, false, true) == BK_NONE && talk_calls == 0);
    press_for(&r, 100, true);                                               // the next press is a normal one
    assert(strcmp(r.log, "PT") == 0);
}

static void test_held_at_start(void)
{
    // BOOT already down when the task starts (held through power-on): ignored until released, then normal.
    rig_t r; rig_init(&r, 0, true);
    talk_answer = true; talk_calls = 0;
    for (int i = 0; i < 40; i++) assert(poll(&r, true, true) == BK_NONE);
    assert(poll(&r, false, true) == BK_NONE);
    assert(talk_calls == 0 && r.n == 0);
    press_for(&r, 100, true);
    assert(strcmp(r.log, "PT") == 0);
    rig_init(&r, 0, false);                                                 // not held at start: a press is a press
    press_for(&r, 100, true);
    assert(strcmp(r.log, "PT") == 0);
}

static void test_clock_wrap(void)
{
    rig_t r; rig_init(&r, UINT32_MAX - 250, false);
    talk_answer = true;
    press_for(&r, 1000, true);                                              // the clock wraps during the hold
    assert(strcmp(r.log, "PSE") == 0);
}

static void test_stop_pending(void)
{
    // The release arrives before the recording began (the start goes through a queue): wait for it, then stop; give up after BK_STOP_WAIT_MS.
    rig_t r; rig_init(&r, 0, false);
    talk_answer = true;
    press_for(&r, 600, true);
    assert(strcmp(r.log, "PSE") == 0);
    assert(!bk_stop_poll(&r.k, r.now, false));
    assert(!bk_stop_poll(&r.k, r.now + 1000, false));                       // still starting
    assert(bk_stop_poll(&r.k, r.now + 1500, true));                         // recording: stop it, once
    assert(!bk_stop_poll(&r.k, r.now + 1600, true));
    // recorded already at the release: stop at once
    rig_init(&r, 0, false);
    press_for(&r, 600, true);
    assert(bk_stop_poll(&r.k, r.now, true));
    assert(!bk_stop_poll(&r.k, r.now + 100, true));
    // never started (not connected): give up quietly, never stop a recording the key did not start
    rig_init(&r, 0, false);
    press_for(&r, 600, true);
    const uint32_t t0 = r.now;
    assert(!bk_stop_poll(&r.k, t0 + BK_STOP_WAIT_MS - 1, false));
    assert(!bk_stop_poll(&r.k, t0 + BK_STOP_WAIT_MS, false));               // gave up
    assert(!bk_stop_poll(&r.k, t0 + BK_STOP_WAIT_MS + 100, true));          // a recording that begins later is not ours to stop
    // a new press cancels a pending stop
    rig_init(&r, 0, false);
    press_for(&r, 600, true);
    poll(&r, true, true);
    assert(!bk_stop_poll(&r.k, r.now, true));
    // a tap never arms it
    rig_init(&r, 0, false);
    press_for(&r, 100, true);
    assert(!bk_stop_poll(&r.k, r.now, true));
}

static void test_reset_countdown(void)
{
    bkr_t c;
    bkr_out_t o;
    // BOOT up at power-on: nothing starts
    assert(!bkr_begin(&c, 1000, false));
    // held at power-on: counts 5 4 3 2 1 and wipes at 5 s
    assert(bkr_begin(&c, 1000, true));
    o = bkr_step(&c, 1000, true); assert(o.result == BKR_COUNTING && o.seconds_left == 5 && o.changed);
    o = bkr_step(&c, 1100, true); assert(o.result == BKR_COUNTING && o.seconds_left == 5 && !o.changed);
    o = bkr_step(&c, 1999, true); assert(o.result == BKR_COUNTING && o.seconds_left == 5 && !o.changed);
    o = bkr_step(&c, 2000, true); assert(o.result == BKR_COUNTING && o.seconds_left == 4 && o.changed);
    o = bkr_step(&c, 3000, true); assert(o.result == BKR_COUNTING && o.seconds_left == 3 && o.changed);
    o = bkr_step(&c, 4000, true); assert(o.result == BKR_COUNTING && o.seconds_left == 2 && o.changed);
    o = bkr_step(&c, 5000, true); assert(o.result == BKR_COUNTING && o.seconds_left == 1 && o.changed);
    o = bkr_step(&c, 5999, true); assert(o.result == BKR_COUNTING && o.seconds_left == 1 && !o.changed);
    o = bkr_step(&c, 6000, true); assert(o.result == BKR_DONE);               // 5000 ms held
    o = bkr_step(&c, 6100, true); assert(o.result == BKR_DONE);               // stays done
    // released early: cancelled, nothing wiped
    assert(bkr_begin(&c, 0, true));
    bkr_step(&c, 0, true); bkr_step(&c, 2500, true);
    o = bkr_step(&c, 2600, false); assert(o.result == BKR_CANCELLED);
    o = bkr_step(&c, 2700, true); assert(o.result == BKR_CANCELLED);          // a later press does not restart it
    // released just before the end
    assert(bkr_begin(&c, 0, true));
    o = bkr_step(&c, 4999, true); assert(o.result == BKR_COUNTING);
    o = bkr_step(&c, 5000, false); assert(o.result == BKR_CANCELLED);         // the release counts first
    // 32-bit clock wrap during the count
    assert(bkr_begin(&c, UINT32_MAX - 1000, true));
    o = bkr_step(&c, UINT32_MAX - 1000, true); assert(o.seconds_left == 5);
    o = bkr_step(&c, 3998, true); assert(o.result == BKR_COUNTING && o.seconds_left == 1);   // 4999 ms later, across the wrap
    o = bkr_step(&c, 3999, true); assert(o.result == BKR_DONE);                               // 5000 ms
    assert(BK_RESET_HOLD_MS == 5000);                                         // 16.1 item 3: at least 5 s
}

int main(void)
{
    test_tap();
    test_hold();
    test_talk_denied();
    test_d1_gate();
    test_held_at_start();
    test_clock_wrap();
    test_stop_pending();
    test_reset_countdown();
    assert(BK_HOLD_MS == 400 && BK_STOP_WAIT_MS == 3000);
    puts("boot key: tap on release, hold-to-talk start and end, talk denied keeps the tap, D1 gate, held at start, stop pending, 5 s reset countdown PASS");
    return 0;
}
