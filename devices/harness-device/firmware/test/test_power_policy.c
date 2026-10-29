// power_policy.c: the battery sleep state machine (S0 external, S1 battery awake, S2 battery asleep), the PMIC read
// schedule, the poller table and the battery screen levels. Pure logic with an injected clock, no ESP-IDF.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "power_policy.h"
#include "board/power.h"

// A small harness: one policy, one input record, a clock. step() advances the clock and takes the fresh PMIC read the
// policy asks for (the glue's loop), exactly as ptt.c does.
typedef struct {
    pp_t p;
    pp_in_t in;
    int pmic;               // what the PMIC would answer right now
    unsigned reads;         // fresh reads taken
    pp_action_t last_action;
    unsigned actions[5];    // count per action
} rig_t;

static void rig_init(rig_t *r, uint32_t start_ms)
{
    memset(r, 0, sizeof *r);
    pp_init(&r->p);
    r->in.now_ms = start_ms;
    r->in.capable = true;
    r->in.chg = -1;
    r->pmic = 2;
}
static pp_state_t step(rig_t *r, uint32_t dt_ms)
{
    r->in.now_ms += dt_ms;
    r->in.chg_fresh = false;
    if (pp_wants_read(&r->p, &r->in)) {
        r->in.chg = r->pmic;
        r->in.chg_fresh = true;
        r->reads++;
    }
    pp_out_t out;
    pp_step(&r->p, &r->in, &out);
    r->last_action = out.action;
    r->actions[out.action]++;
    assert(out.state == r->p.state);
    return out.state;
}
// Run for `ms` in 100 ms ticks.
static pp_state_t run(rig_t *r, uint32_t ms)
{
    pp_state_t s = r->p.state;
    for (uint32_t t = 0; t < ms; t += 100) s = step(r, 100);
    return s;
}
// Bring a rig to S1 (battery, screen on).
static void to_s1(rig_t *r)
{
    r->in.chg = 2;      // the 30 s cache says "discharging"
    run(r, 3000);
    assert(r->p.state == PP_S1);
}
// ... and to S2.
static void to_s2(rig_t *r)
{
    to_s1(r);
    r->in.screen_asleep = true;
    run(r, PP_SETTLE_MS + 200);
    assert(r->p.state == PP_S2);
}

int main(void)
{
    rig_t r;

    // --- AXP2101 status decoding (board/power.h, datasheet V1.4 p.31) ---
    assert(POWER_STATUS0_VBUS_GOOD == (1 << 5) && POWER_STATUS0_BATT_PRESENT == (1 << 3));
    assert(power_chg_from_status2(0x00) == 0 && power_chg_from_status2(0x20) == 1 && power_chg_from_status2(0x40) == 2);   // standby, charge, discharge
    assert(power_chg_from_status2(0x60) == 3);                                    // reserved stays 3 (not "discharging", not a valid state)
    assert(power_chg_from_status2(0xC0) == 2 && power_chg_from_status2(0xA0) == 1 && power_chg_from_status2(0x80) == 0);   // bit 7 is reserved: masked off
    assert(power_chg_from_status2(0x5F) == 2 && power_chg_from_status2(0x1F) == 0);                    // the low bits (charging status) do not leak in
    assert(power_chg_from_status2(-1) == -1);

    // --- S0 is the resting state, and the cheap one ---
    rig_init(&r, 1000);
    assert(r.p.state == PP_S0 && !pp_wants_read(&r.p, &r.in));      // nothing known: no PMIC traffic at all
    // A charger (0 standby, 1 charging) for an hour: never leaves S0, never reads the PMIC (the cache says not 2).
    r.in.chg = 1;
    for (int i = 0; i < 36000; i++) assert(step(&r, 100) == PP_S0);
    assert(r.reads == 0 && r.actions[PP_ACT_NONE] == 36000);
    r.in.chg = 0;
    for (int i = 0; i < 36000; i++) assert(step(&r, 100) == PP_S0);
    assert(r.reads == 0);
    // Unknown (-1) stays in S0 as well.
    r.in.chg = -1;
    for (int i = 0; i < 1000; i++) assert(step(&r, 100) == PP_S0);
    assert(r.reads == 0);
    // A board without the PMIC (or the CST9217) is S0 whatever the numbers say.
    rig_init(&r, 5);
    r.in.capable = false; r.in.chg = 2;
    for (int i = 0; i < 1000; i++) assert(step(&r, 100) == PP_S0);
    assert(r.reads == 0);

    // --- S0 to S1 needs two fresh reads 2 s apart ---
    rig_init(&r, 0);
    r.in.chg = 2;                                  // the cache says discharging: now it is worth reading
    assert(pp_wants_read(&r.p, &r.in));            // the first read is due at once
    assert(step(&r, 100) == PP_S0 && r.reads == 1);// one read is not enough
    assert(!pp_wants_read(&r.p, &r.in));
    for (int i = 0; i < 19; i++) assert(step(&r, 100) == PP_S0);   // up to 1.9 s after the first read: still S0
    assert(r.reads == 1);
    assert(step(&r, 100) == PP_S1);                // 2.0 s: the second read confirms
    assert(r.reads == 2 && r.last_action == PP_ACT_ENTER_S1);
    // Exactly 1999 ms between the reads is not enough (a direct step sequence, no wants_read gating).
    {
        pp_t p; pp_init(&p);
        pp_in_t in = {.now_ms = 0, .chg = 2, .chg_fresh = true, .capable = true};
        pp_out_t out;
        pp_step(&p, &in, &out); assert(out.state == PP_S0);
        in.now_ms = 1999; pp_step(&p, &in, &out); assert(out.state == PP_S0 && out.action == PP_ACT_NONE);
        in.now_ms = 2000; pp_step(&p, &in, &out); assert(out.state == PP_S1);
    }
    // A fresh read that is not 2 in between restarts the count.
    rig_init(&r, 0);
    r.in.chg = 2;
    step(&r, 100);                                 // read 1: 2
    r.pmic = 1;
    run(&r, 2000);                                 // read 2: 1 (charging)
    assert(r.p.state == PP_S0);
    r.pmic = 2; r.in.chg = 2;
    run(&r, 3000);
    assert(r.p.state == PP_S0);                    // the new first read was taken (2 s after the last); the second is not due
    run(&r, 1000);
    assert(r.p.state == PP_S1);
    // A USB host blocks S1 even with "discharging" (a flaky PMIC read on a host cable).
    rig_init(&r, 0);
    r.in.chg = 2; r.in.sof = true;
    run(&r, 10000);
    assert(r.p.state == PP_S0 && r.reads == 0);
    r.in.sof = false;
    run(&r, 3000);
    assert(r.p.state == PP_S1);

    // --- S1 to S2: the screen must stay off for 3 s ---
    rig_init(&r, 0); to_s1(&r);
    r.in.screen_asleep = true;
    {
        uint32_t t0 = r.in.now_ms;
        pp_state_t s = PP_S1;
        // the policy sees the asleep screen for the first time on the next step; count from there
        s = step(&r, 100); assert(s == PP_S1);
        uint32_t armed_at = r.in.now_ms;
        while (r.in.now_ms - armed_at < PP_SETTLE_MS - 100) assert(step(&r, 100) == PP_S1);
        assert(r.in.now_ms - armed_at == PP_SETTLE_MS - 100);          // 2.9 s asleep: still S1
        assert(step(&r, 100) == PP_S2);                                  // 3.0 s: S2
        assert(r.last_action == PP_ACT_ENTER_S2);
        (void)t0;
    }
    // The screen waking mid-settle cancels the count; the next sleep starts from zero.
    rig_init(&r, 0); to_s1(&r);
    r.in.screen_asleep = true; run(&r, 2000);
    r.in.screen_asleep = false; run(&r, 100);
    r.in.screen_asleep = true; run(&r, 2000);
    assert(r.p.state == PP_S1);                    // 2 s since the new sleep, not 4
    run(&r, 1200);
    assert(r.p.state == PP_S2);
    // A hold bit blocks S2; clearing it lets an already-settled sleep through at once.
    rig_init(&r, 0); to_s1(&r);
    r.in.screen_asleep = true; r.in.holds = PP_HOLD_VOICE;
    run(&r, 20000);
    assert(r.p.state == PP_S1 && r.actions[PP_ACT_ENTER_S2] == 0);
    r.in.holds = 0;
    step(&r, 100);
    assert(r.p.state == PP_S2);
    // A hold bit set in S2 pulls the dial back to S1 (the glue takes the awake lock back).
    r.in.holds = PP_HOLD_FWUPDATE;
    assert(step(&r, 100) == PP_S1 && r.last_action == PP_ACT_LEAVE_S2);
    r.in.holds = PP_HOLD_SESSION | PP_HOLD_LINK;
    run(&r, 5000); assert(r.p.state == PP_S1);

    // --- S2 to S1 when the screen wakes; and back ---
    rig_init(&r, 0); to_s2(&r);
    r.in.screen_asleep = false;
    assert(step(&r, 100) == PP_S1 && r.last_action == PP_ACT_LEAVE_S2);
    r.in.screen_asleep = true;
    run(&r, 5000);
    assert(r.p.state == PP_S2);
    assert(r.p.s2_entries == 2);

    // --- Anything unsafe goes to S0 at once, from S1 and from S2 ---
    // a USB host
    rig_init(&r, 0); to_s1(&r);
    r.in.sof = true;
    assert(step(&r, 100) == PP_S0 && r.last_action == PP_ACT_ENTER_S0);
    rig_init(&r, 0); to_s2(&r);
    r.in.sof = true;
    {
        pp_out_t out; r.in.now_ms += 100; r.in.chg_fresh = false; pp_step(&r.p, &r.in, &out);
        assert(out.state == PP_S0 && out.action == PP_ACT_ENTER_S0 && out.from == PP_S2);
    }
    // a fresh read of "not discharging" (plugged in), and of "unknown" (never sleep on a doubt)
    for (int bad = -1; bad <= 1; bad++) {
        rig_init(&r, 0); to_s2(&r);
        r.pmic = bad;
        pp_state_t s = r.p.state;
        for (int i = 0; i < 25 && s == PP_S2; i++) s = step(&r, 100);
        assert(s == PP_S0 && r.last_action == PP_ACT_ENTER_S0);
    }
    // plug-in is noticed within the 2 s read interval (+ one poll)
    rig_init(&r, 0); to_s2(&r);
    r.pmic = 1;
    {
        uint32_t t0 = r.in.now_ms; pp_state_t s = PP_S2;
        while (s == PP_S2) s = step(&r, 100);
        assert(r.in.now_ms - t0 <= PP_PMIC_READ_MS + 100);
    }
    // a stale cache value never leaves S1 or S2 (only a fresh read does): the PMIC is still read on its own schedule
    rig_init(&r, 0); to_s2(&r);
    r.in.chg = 0; r.in.chg_fresh = false;
    {
        pp_out_t out; r.in.now_ms += 100; pp_step(&r.p, &r.in, &out);
        assert(out.state == PP_S2 && out.action == PP_ACT_NONE);
    }
    // no PMIC capability at all
    rig_init(&r, 0); to_s1(&r);
    r.in.capable = false;
    assert(step(&r, 100) == PP_S0);

    // --- The PMIC read schedule in S1 and S2: every 2 s, no more ---
    rig_init(&r, 0); to_s2(&r);
    unsigned before = r.reads;
    run(&r, 20000);
    assert(r.reads - before >= 9 && r.reads - before <= 10);
    assert(r.p.state == PP_S2);

    // --- Anti-flap: a drop to S0 holds off S1 for 10 s ---
    rig_init(&r, 0); to_s1(&r);
    r.pmic = 1;
    {
        pp_state_t s = PP_S1;
        for (int i = 0; i < 30 && s == PP_S1; i++) s = step(&r, 100);
        assert(s == PP_S0);
    }
    uint32_t dropped_at = r.in.now_ms;
    r.pmic = 2; r.in.chg = 2;                         // flickers back to "discharging" at once
    while (r.in.now_ms - dropped_at < PP_DWELL_MS - 100) assert(step(&r, 100) == PP_S0);
    assert(r.in.now_ms - dropped_at == PP_DWELL_MS - 100);
    run(&r, 100);                                    // 10 s reached; the confirmation reads then take their 2 s
    assert(r.p.state == PP_S0);
    run(&r, 3000);
    assert(r.p.state == PP_S1);
    // The dwell is for drops to S0 only; a fresh boot is not held back.
    rig_init(&r, 0); r.in.chg = 2;
    run(&r, 2100); assert(r.p.state == PP_S1);

    // --- The 32-bit clock wraps without a hiccup ---
    rig_init(&r, UINT32_MAX - 1500);
    r.in.chg = 2;
    run(&r, 2500);                                   // crosses the wrap during the confirmation
    assert(r.p.state == PP_S1);
    r.in.screen_asleep = true;
    run(&r, PP_SETTLE_MS + 300);
    assert(r.p.state == PP_S2);
    rig_init(&r, UINT32_MAX - 2000);
    to_s1(&r);
    r.in.screen_asleep = true;
    run(&r, 2800); assert(r.p.state == PP_S1);       // 2.8 s asleep, the clock wrapped in the middle
    run(&r, 400);  assert(r.p.state == PP_S2);

    // --- Poll hints per state ---
    assert(pp_ptt_poll_ms(PP_S0) == 100 && pp_ptt_poll_ms(PP_S1) == 100 && pp_ptt_poll_ms(PP_S2) == 250);
    assert(!strcmp(pp_state_name(PP_S0), "S0") && !strcmp(pp_state_name(PP_S1), "S1") && !strcmp(pp_state_name(PP_S2), "S2"));
    assert(!strcmp(pp_state_name((pp_state_t)9), "S?"));

    // --- The poller table: S0 and S1 give back the caller's own wait; S2 the relaxed one ---
    static const struct { pp_poller_t who; uint32_t awake, relaxed; } rows[] = {
        {PP_POLLER_TOUCH, 20, 1000}, {PP_POLLER_RENDER, 1000, 5000}, {PP_POLLER_CABLE_READER, 100, 2000},
        {PP_POLLER_CABLE_SESSION, 250, 2000}, {PP_POLLER_REFRESH, 1000, 5000}, {PP_POLLER_BATLOG, 1000, 10000},
    };
    assert(sizeof rows / sizeof rows[0] == PP_POLLER_COUNT);
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        assert(pp_wait_ms(PP_S0, rows[i].who, rows[i].awake) == rows[i].awake);
        assert(pp_wait_ms(PP_S1, rows[i].who, rows[i].awake) == rows[i].awake);
        assert(pp_wait_ms(PP_S2, rows[i].who, rows[i].awake) == rows[i].relaxed);
        assert(pp_relaxed_ms(rows[i].who) == rows[i].relaxed);
        // A caller that already waits longer than the relaxed value keeps its own (never shortens a wait in S2).
        assert(pp_wait_ms(PP_S2, rows[i].who, 60000) == 60000);
    }
    assert(PP_TOUCH_POLL_MS == 1000);
    assert(pp_wait_ms(PP_S2, PP_POLLER_COUNT, 77) == 77);   // an unknown poller is left alone

    // --- The battery screen levels ---
    // on external power (S0): off after 300 s, strictly
    assert(pp_screen_level(PP_S0, 0) == PP_SCREEN_FULL);
    assert(pp_screen_level(PP_S0, 59999) == PP_SCREEN_FULL);
    assert(pp_screen_level(PP_S0, 60001) == PP_SCREEN_FULL);   // no dimming on USB
    assert(pp_screen_level(PP_S0, 300000) == PP_SCREEN_FULL);
    assert(pp_screen_level(PP_S0, 300001) == PP_SCREEN_OFF);
    // on the battery: dim after 30 s, off after 60 s
    for (pp_state_t s = PP_S1; s <= PP_S2; s++) {
        assert(pp_screen_level(s, 0) == PP_SCREEN_FULL);
        assert(pp_screen_level(s, 30000) == PP_SCREEN_FULL);
        assert(pp_screen_level(s, 30001) == PP_SCREEN_DIM);
        assert(pp_screen_level(s, 60000) == PP_SCREEN_DIM);
        assert(pp_screen_level(s, 60001) == PP_SCREEN_OFF);
        assert(pp_screen_level(s, 4000000000u) == PP_SCREEN_OFF);
    }
    assert(pp_dimmed_percent(100) == 25 && pp_dimmed_percent(40) == 10 && pp_dimmed_percent(8) == 3 && pp_dimmed_percent(0) == 3);
    assert(pp_dimmed_percent(12) == 3 && pp_dimmed_percent(16) == 4);

    puts("power policy: S0/S1/S2 transitions, PMIC read schedule, hold bits, anti-flap, 32-bit clock wrap, poller table, battery screen levels PASS");
    return 0;
}
