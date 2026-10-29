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
// P press, T tap, S hold start, E hold end, L long press (the screen toggle of a board without the PWR key).
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
    if (e != BK_NONE) r->log[r->n++] = "-PTSEL"[e];
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

static void test_no_pwr_key_board(void)
{
    // A board without the PMIC has no PWR key (board().has_pmic false): there BOOT keeps its old second job, a hold of BK_TOGGLE_MS toggles the
    // screen, and there is no hold-to-talk. A tap is still a tap, on the release.
    rig_t r;
    rig_init(&r, 0, false);
    bk_init_ex(&r.k, false, true);
    talk_answer = true; talk_calls = 0;
    press_for(&r, 300, true);
    assert(strcmp(r.log, "PT") == 0);
    r.n = 0; memset(r.log, 0, sizeof r.log);
    for (int i = 0; i < 7; i++) assert(poll(&r, true, true) == (i == 0 ? BK_PRESS : BK_NONE));   // the press edge, then 600 ms down
    assert(poll(&r, true, true) == BK_NONE);                                // 700 ms down
    assert(poll(&r, true, true) == BK_LONG);                                // 800 ms down: the screen toggles
    for (int i = 0; i < 20; i++) assert(poll(&r, true, true) == BK_NONE);   // once per press
    assert(poll(&r, false, true) == BK_NONE);                               // the release after a long press is not a tap, and sends nothing
    assert(talk_calls == 0 && !bk_capture_active(&r.k));
    // the D1 gate still ignores a press on a dark battery screen, and a key held through power-on waits for its release
    rig_init(&r, 0, false);
    bk_init_ex(&r.k, false, true);
    press_for(&r, 3000, false);
    assert(r.n == 0);
    rig_init(&r, 0, true);
    bk_init_ex(&r.k, true, true);
    for (int i = 0; i < 20; i++) assert(poll(&r, true, true) == BK_NONE);
    assert(poll(&r, false, true) == BK_NONE);
    // the dial's machine never says BK_LONG
    rig_init(&r, 0, false);
    press_for(&r, 3000, true);
    assert(strchr(r.log, 'L') == NULL);
    assert(BK_TOGGLE_MS == 800);
}

static void test_clock_wrap(void)
{
    rig_t r; rig_init(&r, UINT32_MAX - 250, false);
    talk_answer = true;
    press_for(&r, 1000, true);                                              // the clock wraps during the hold
    assert(strcmp(r.log, "PSE") == 0);
}

// ---- the capture a hold started: send it, or cancel it if it is too short, and never touch another one ----
// Run a hold to its start (press at 100 ms, start at 500 ms) with a capture token, then the caller drives the polls.
static void hold_to_start(rig_t *r, uint32_t token)
{
    talk_answer = true;
    rig_init(r, 0, false);
    for (int i = 0; i < 4; i++) poll(r, true, true);                 // 100 press, 200, 300, 400
    assert(poll(r, true, true) == BK_HOLD_START);                    // 500
    bk_capture_begin(&r->k, token);
    assert(bk_capture_active(&r->k));
}
static bk_cap_t cap(rig_t *r, bool recording, uint32_t token) { return bk_capture_poll(&r->k, r->now, recording, token); }

static void test_capture(void)
{
    rig_t r;
    // (a) a normal talk: recording begins during the hold, the release sends it
    hold_to_start(&r, 7);
    assert(poll(&r, true, true) == BK_NONE && cap(&r, false, 7) == BK_CAP_NONE);   // 600: still starting
    assert(poll(&r, true, true) == BK_NONE && cap(&r, true, 7) == BK_CAP_NONE);    // 700: recording began (seen here)
    for (int i = 0; i < 8; i++) { poll(&r, true, true); assert(cap(&r, true, 7) == BK_CAP_NONE); }   // held on: nothing yet
    assert(poll(&r, false, true) == BK_HOLD_END);                     // 1500: 800 ms of recording
    assert(cap(&r, true, 7) == BK_CAP_SEND);
    assert(cap(&r, true, 7) == BK_CAP_NONE && !bk_capture_active(&r.k));   // once

    // (b) the minimum clip: released less than BK_MIN_CLIP_MS after recording began is cancelled, not sent. Began at 700.
    assert(BK_MIN_CLIP_MS == 700);
    const struct { int polls_after_began; bk_cap_t want; } rows[] = {{1, BK_CAP_CANCEL}, {6, BK_CAP_CANCEL}, {7, BK_CAP_SEND}, {8, BK_CAP_SEND}};
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        hold_to_start(&r, 9);
        poll(&r, true, true); cap(&r, false, 9);                      // 600
        poll(&r, true, true); cap(&r, true, 9);                       // 700: began
        for (int k = 1; k < rows[i].polls_after_began; k++) { poll(&r, true, true); cap(&r, true, 9); }
        assert(poll(&r, false, true) == BK_HOLD_END);                 // the release, polls_after_began * 100 ms after it began
        assert(cap(&r, true, 9) == rows[i].want);
    }
    // (c) released before the capture began: when it does begin it has recorded 0 ms, so it is cancelled
    hold_to_start(&r, 11);
    assert(poll(&r, false, true) == BK_HOLD_END);                     // 600
    assert(cap(&r, false, 11) == BK_CAP_NONE);
    for (int i = 0; i < 5; i++) { poll(&r, false, true); assert(cap(&r, false, 11) == BK_CAP_NONE); }
    poll(&r, false, true);
    assert(cap(&r, true, 11) == BK_CAP_CANCEL);
    // (d) it never began (not connected): give up quietly after BK_STOP_WAIT_MS, and a recording that begins later is not ours to touch
    hold_to_start(&r, 12);
    poll(&r, false, true);
    const uint32_t t0 = r.now;
    assert(bk_capture_poll(&r.k, t0 + BK_STOP_WAIT_MS - 1, false, 12) == BK_CAP_NONE && bk_capture_active(&r.k));
    assert(bk_capture_poll(&r.k, t0 + BK_STOP_WAIT_MS, false, 12) == BK_CAP_NONE && !bk_capture_active(&r.k));
    assert(bk_capture_poll(&r.k, t0 + BK_STOP_WAIT_MS + 100, true, 12) == BK_CAP_NONE);
    // (e) ANOTHER CAPTURE: the token moved on (a touch started one, or this one was closed). Nothing is sent or cancelled, ever.
    hold_to_start(&r, 20);
    poll(&r, true, true);
    assert(cap(&r, true, 21) == BK_CAP_NONE && !bk_capture_active(&r.k));   // the capture the hold started is gone
    assert(poll(&r, false, true) == BK_HOLD_END);
    assert(cap(&r, true, 21) == BK_CAP_NONE && cap(&r, true, 20) == BK_CAP_NONE);
    // the same when the token moves after the release
    hold_to_start(&r, 30);
    poll(&r, true, true); cap(&r, true, 30);
    assert(poll(&r, false, true) == BK_HOLD_END);
    assert(cap(&r, true, 31) == BK_CAP_NONE && !bk_capture_active(&r.k));
    // (f) a hold whose start did not take (no token): its release has nothing to send, whatever is recording
    talk_answer = true;
    rig_init(&r, 0, false);
    for (int i = 0; i < 5; i++) poll(&r, true, true);
    assert(r.log[1] == 'S' && !bk_capture_active(&r.k));
    assert(poll(&r, false, true) == BK_HOLD_END);
    assert(cap(&r, true, 99) == BK_CAP_NONE && cap(&r, true, 0) == BK_CAP_NONE);
    // (g) a new press ends the wait for the old capture
    hold_to_start(&r, 40);
    assert(poll(&r, false, true) == BK_HOLD_END);
    assert(poll(&r, true, true) == BK_PRESS);
    assert(cap(&r, true, 40) == BK_CAP_NONE && !bk_capture_active(&r.k));
    // (h) the capture ended by itself (the 30 s limit) while the key was still down: the release finds nothing to stop
    hold_to_start(&r, 50);
    poll(&r, true, true); cap(&r, true, 50);
    poll(&r, true, true); cap(&r, false, 50);                         // recording was seen, now it is not
    assert(poll(&r, false, true) == BK_HOLD_END);
    assert(cap(&r, false, 50) == BK_CAP_NONE && !bk_capture_active(&r.k));
    // (i) a tap never has a capture
    rig_init(&r, 0, false);
    press_for(&r, 100, true);
    assert(!bk_capture_active(&r.k) && cap(&r, true, 1) == BK_CAP_NONE);
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
    test_no_pwr_key_board();
    test_capture();
    test_reset_countdown();
    assert(BK_HOLD_MS == 400 && BK_STOP_WAIT_MS == 3000);
    puts("boot key: tap on release, hold-to-talk start and end, talk denied keeps the tap, D1 gate, held at start, capture token, minimum clip, 5 s reset countdown PASS");
    return 0;
}
