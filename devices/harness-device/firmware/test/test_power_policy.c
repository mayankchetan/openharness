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
    int pmic;               // what the PMIC would answer right now (REG 0x01 direction: 0 standby, 1 charge, 2 discharge, -1 read error)
    int status0;            // ... and REG 0x00 (bit 5 VBUS good, bit 3 battery present; -1 read error). Default: on battery.
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
    r->status0 = PP_PMU_BATT_PRESENT;
}
static pp_state_t step(rig_t *r, uint32_t dt_ms)
{
    r->in.now_ms += dt_ms;
    r->in.chg_fresh = false;
    if (pp_wants_read(&r->p, &r->in)) {
        r->in.chg = r->pmic;
        r->in.status0 = r->status0;
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

// ---- the power-off triggers ----
typedef struct { pp_off_t o; pp_off_in_t in; } offrig_t;
static void offrig_init(offrig_t *r, uint32_t start, pp_state_t st, int pct)
{
    memset(r, 0, sizeof *r);
    pp_off_init(&r->o);
    r->in = (pp_off_in_t){.now_ms = start, .state = st, .on_battery = true, .sof = false, .pct = pct};
}
// One step `dt` ms later.
static pp_off_reason_t offstep(offrig_t *r, uint32_t dt) { r->in.now_ms += dt; return pp_off_step(&r->o, &r->in); }
// Run for `ms` in 1 s steps; returns the first reason seen (PP_OFF_NONE if none) and leaves the clock where it stopped.
static pp_off_reason_t offrun(offrig_t *r, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 1000) {
        const pp_off_reason_t e = offstep(r, 1000);
        if (e != PP_OFF_NONE) return e;
    }
    return PP_OFF_NONE;
}

static void test_poweroff_triggers(void)
{
    // the constants the plan and the user decided (the drain threshold is provisional until the first long run)
    assert(PP_OFF_S2_MS == 30u * 60u * 1000u && PP_OFF_LOW_PCT == 5 && PP_OFF_DRAIN_PCT_PER_H == 15);
    assert(!strcmp(pp_off_reason_name(PP_OFF_NONE), "none") && !strcmp(pp_off_reason_name(PP_OFF_LOW_BATTERY), "low_battery") &&
           !strcmp(pp_off_reason_name(PP_OFF_DRAIN), "drain") && !strcmp(pp_off_reason_name(PP_OFF_S2_TIMEOUT), "s2_30min"));
    offrig_t r;

    // (a) 30 min continuously in S2
    offrig_init(&r, 5000, PP_S2, 80);
    assert(offstep(&r, 0) == PP_OFF_NONE);                                    // the first S2 step starts the clock
    assert(offrun(&r, PP_OFF_S2_MS - 1000) == PP_OFF_NONE);                   // 29 min 59 s
    assert(offstep(&r, 999) == PP_OFF_NONE);                                  // 1,799,999 ms
    assert(offstep(&r, 1) == PP_OFF_S2_TIMEOUT);                              // 1,800,000 ms
    assert(offstep(&r, 1000) == PP_OFF_S2_TIMEOUT);                           // and it stays asked for until the dial is off or the state changes
    // a visit to S1 (the screen woke) restarts the 30 min
    offrig_init(&r, 5000, PP_S2, 80);
    assert(offrun(&r, 20u * 60u * 1000u) == PP_OFF_NONE);
    r.in.state = PP_S1; assert(offstep(&r, 1000) == PP_OFF_NONE);
    r.in.state = PP_S2; assert(offstep(&r, 1000) == PP_OFF_NONE);
    assert(offrun(&r, 29u * 60u * 1000u) == PP_OFF_NONE);                     // 29 min since the return: not yet
    assert(offrun(&r, 2u * 60u * 1000u) == PP_OFF_S2_TIMEOUT);
    // never from S1, however long the screen is on
    offrig_init(&r, 0, PP_S1, 80);
    assert(offrun(&r, 3u * 3600u * 1000u) == PP_OFF_NONE);
    // the 32-bit clock wraps in the middle of the 30 min
    offrig_init(&r, UINT32_MAX - 600000u, PP_S2, 80);
    assert(offstep(&r, 0) == PP_OFF_NONE && offrun(&r, PP_OFF_S2_MS - 1000) == PP_OFF_NONE);
    assert(offstep(&r, 1000) == PP_OFF_S2_TIMEOUT);

    // (b) the cell is flat: at or below 5 percent for a full minute in S2
    offrig_init(&r, 0, PP_S2, 5);
    assert(offstep(&r, 0) == PP_OFF_NONE && offrun(&r, PP_OFF_LOW_HOLD_MS - 1000) == PP_OFF_NONE);
    assert(offstep(&r, 999) == PP_OFF_NONE && offstep(&r, 1) == PP_OFF_LOW_BATTERY);
    offrig_init(&r, 0, PP_S2, 6);                                             // 6 percent is not low
    assert(offrun(&r, 10u * 60u * 1000u) == PP_OFF_NONE);
    offrig_init(&r, 0, PP_S2, 5);                                             // a reading above 5 or an unknown one breaks the minute
    offrun(&r, 50000); r.in.pct = 6; offstep(&r, 1000); r.in.pct = 5;
    assert(offrun(&r, PP_OFF_LOW_HOLD_MS - 1000) == PP_OFF_NONE && offrun(&r, 2000) == PP_OFF_LOW_BATTERY);
    offrig_init(&r, 0, PP_S2, 5);
    offrun(&r, 50000); r.in.pct = -1; offstep(&r, 1000); r.in.pct = 5;
    assert(offrun(&r, PP_OFF_LOW_HOLD_MS - 1000) == PP_OFF_NONE);
    offrig_init(&r, 0, PP_S2, 0);                                             // a gauge that says 0 counts as low once it has held for the minute
    assert(offrun(&r, PP_OFF_LOW_HOLD_MS + 1000) == PP_OFF_LOW_BATTERY);
    offrig_init(&r, 0, PP_S1, 3);                                             // only from S2: the screen is on, the user sees the rim
    assert(offrun(&r, 10u * 60u * 1000u) == PP_OFF_NONE);

    // (c) the drain, measured on S2 samples only: one gauge sample a minute while in S2, the history starts again every time S2 is left,
    // and nothing is judged before PP_OFF_DRAIN_MIN_MS (20 min) of unbroken S2. The rate is the fall over those minutes, scaled to an hour.
    assert(PP_OFF_DRAIN_MIN_MS == 20u * 60u * 1000u);
    {
        // 30 percent an hour (one point every 2 minutes): asked for at 20 minutes, when the 21st sample is in, not before
        offrig_init(&r, 0, PP_S2, 90);
        pp_off_reason_t e = PP_OFF_NONE;
        uint32_t minutes = 0;
        for (; minutes < 19 && e == PP_OFF_NONE; minutes++) {
            if (minutes % 2 == 0 && minutes) r.in.pct--;
            e = offrun(&r, 60000);
        }
        assert(e == PP_OFF_NONE && pp_off_drain_per_hour(&r.o) < 0);          // 19 minutes of S2: nothing to judge yet, however fast it falls
        for (; minutes < 22 && e == PP_OFF_NONE; minutes++) {
            if (minutes % 2 == 0) r.in.pct--;
            e = offrun(&r, 60000);
        }
        assert(e == PP_OFF_DRAIN && pp_off_drain_per_hour(&r.o) >= 27);
    }
    // the threshold: exactly 15 an hour is not above it, 18 is. 5 points in the 20 minutes is 15 an hour, 6 is 18.
    for (int drop = 5; drop <= 6; drop++) {
        offrig_init(&r, 0, PP_S2, 90);
        offrun(&r, 60000);                                                    // the first sample, 90
        r.in.pct = 90 - drop;
        for (int m = 0; m < 19; m++) assert(offrun(&r, 60000) == PP_OFF_NONE);   // 20 samples: 19 minutes of history
        assert(pp_off_drain_per_hour(&r.o) < 0);
        pp_off_reason_t e = offrun(&r, 60000);                                // the 21st sample: 20 minutes of history
        assert(pp_off_drain_per_hour(&r.o) == drop * 3);
        assert(e == (drop == 5 ? PP_OFF_NONE : PP_OFF_DRAIN));
    }
    // a slow fall (12 an hour) never asks for it by the drain: the 30 min is what ends that S2
    offrig_init(&r, 0, PP_S2, 90);
    for (int m = 0; m < 29; m++) { if (m % 5 == 0 && m) r.in.pct--; assert(offrun(&r, 60000) == PP_OFF_NONE); }
    assert(offrun(&r, 90000) == PP_OFF_S2_TIMEOUT);
    // AN HOUR OF S1 DOES NOT COUNT: a screen-on hour on the battery with the gauge falling 30 points, then the screen goes off. The first
    // S2 poll, and every poll for the next 19 minutes, asks for nothing.
    offrig_init(&r, 0, PP_S1, 90);
    for (int m = 0; m < 62; m++) { if (m % 2 == 0 && m) r.in.pct--; assert(offrun(&r, 60000) == PP_OFF_NONE); }
    assert(pp_off_drain_per_hour(&r.o) < 0);                                  // no S1 sample is kept
    r.in.state = PP_S2;
    assert(offstep(&r, 0) == PP_OFF_NONE && offstep(&r, 1000) == PP_OFF_NONE);   // the first S2 polls
    for (int m = 0; m < 18; m++) { r.in.pct -= (m % 2 == 0); assert(offrun(&r, 60000) == PP_OFF_NONE); }
    // a visit to S1 in the middle of S2 starts the history again
    offrig_init(&r, 0, PP_S2, 90);
    for (int m = 0; m < 15; m++) { r.in.pct -= (m % 2); assert(offrun(&r, 60000) == PP_OFF_NONE); }
    r.in.state = PP_S1; offstep(&r, 1000);
    assert(pp_off_drain_per_hour(&r.o) < 0);
    r.in.state = PP_S2;
    for (int m = 0; m < 18; m++) { r.in.pct -= (m % 2 == 0); assert(offrun(&r, 60000) == PP_OFF_NONE); }   // 18 minutes since the return
    // external power throws the history away
    offrig_init(&r, 0, PP_S2, 90);
    for (int m = 0; m < 19; m++) { r.in.pct -= (m % 2 == 0); offrun(&r, 60000); }
    r.in.state = PP_S0; r.in.on_battery = false; offstep(&r, 1000);
    r.in.state = PP_S2; r.in.on_battery = true;
    assert(pp_off_drain_per_hour(&r.o) < 0 && offstep(&r, 1000) == PP_OFF_NONE);

    // (d) NEVER on external power: VBUS good (not on the battery), a USB host, or S0 -- however long and however low
    offrig_init(&r, 0, PP_S2, 1);
    r.in.on_battery = false;
    assert(offrun(&r, 3u * 3600u * 1000u) == PP_OFF_NONE);
    offrig_init(&r, 0, PP_S2, 1);
    r.in.sof = true;
    assert(offrun(&r, 3u * 3600u * 1000u) == PP_OFF_NONE);
    offrig_init(&r, 0, PP_S0, 1);
    assert(offrun(&r, 3u * 3600u * 1000u) == PP_OFF_NONE);
    // a plug-in during the 30 min throws the elapsed time away: it is not carried over to the next battery spell
    offrig_init(&r, 0, PP_S2, 80);
    offrun(&r, 29u * 60u * 1000u);
    r.in.on_battery = false; assert(offstep(&r, 1000) == PP_OFF_NONE);
    r.in.on_battery = true;  assert(offstep(&r, 1000) == PP_OFF_NONE);
    assert(offrun(&r, 5u * 60u * 1000u) == PP_OFF_NONE);                      // 5 minutes after the return, not 1
    // a plug-in in the middle of the flat-cell minute likewise
    offrig_init(&r, 0, PP_S2, 4);
    offrun(&r, 50000);
    r.in.sof = true; assert(offstep(&r, 1000) == PP_OFF_NONE);
    r.in.sof = false;
    assert(offrun(&r, 30000) == PP_OFF_NONE);

    // (e) priority when several hold at once: a flat cell first
    offrig_init(&r, 0, PP_S2, 4);
    assert(offrun(&r, PP_OFF_S2_MS + 1000) == PP_OFF_LOW_BATTERY);            // 60 s into S2 at 4 percent: low wins long before 30 min
    // ... then the drain, before the 30 min: 40 an hour, asked for at 20 minutes of S2
    offrig_init(&r, 0, PP_S2, 90);
    pp_off_reason_t e = PP_OFF_NONE;
    for (int m = 0; m < 28 && e == PP_OFF_NONE; m++) { if (m) r.in.pct -= (m % 3 != 0) ? 1 : 0; e = offrun(&r, 60000); }
    assert(e == PP_OFF_DRAIN && r.in.now_ms >= PP_OFF_DRAIN_MIN_MS && r.in.now_ms < PP_OFF_S2_MS);
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
    assert(PP_PMU_VBUS_GOOD == POWER_STATUS0_VBUS_GOOD && PP_PMU_BATT_PRESENT == POWER_STATUS0_BATT_PRESENT);   // the policy's copies agree

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
        pp_in_t in = {.now_ms = 0, .chg = 2, .chg_fresh = true, .capable = true, .status0 = PP_PMU_BATT_PRESENT};
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

    // --- "On the battery" is three facts on one fresh read: VBUS not good, battery present, direction discharge ---
    // VBUS good with a discharging direction (a charger that is not charging) stays in S0, forever, and the PMIC is still read every 2 s
    rig_init(&r, 0); r.status0 = PP_PMU_VBUS_GOOD | PP_PMU_BATT_PRESENT; r.in.chg = 2;
    run(&r, 60000);
    assert(r.p.state == PP_S0 && r.reads >= 29 && r.actions[PP_ACT_ENTER_S1] == 0);
    // no battery (bit 3 clear): S0
    rig_init(&r, 0); r.status0 = 0; r.in.chg = 2;
    run(&r, 60000);
    assert(r.p.state == PP_S0 && r.actions[PP_ACT_ENTER_S1] == 0);
    // a failed read of REG 0x00 (-1) with a good direction: S0, and it recovers when the read does
    rig_init(&r, 0); r.status0 = -1; r.in.chg = 2;
    run(&r, 30000);
    assert(r.p.state == PP_S0 && r.actions[PP_ACT_ENTER_S1] == 0);
    r.status0 = PP_PMU_BATT_PRESENT;
    run(&r, 2500);
    assert(r.p.state == PP_S1);
    // an interrupted confirmation starts again: VBUS good on the second read
    rig_init(&r, 0); r.in.chg = 2;
    step(&r, 100); r.status0 = PP_PMU_VBUS_GOOD | PP_PMU_BATT_PRESENT;
    run(&r, 2100); assert(r.p.state == PP_S0);
    r.status0 = PP_PMU_BATT_PRESENT; run(&r, 1900); assert(r.p.state == PP_S0);   // the count restarted at the VBUS-good read
    run(&r, 3000); assert(r.p.state == PP_S1);
    // from S1 and S2, one fresh read that fails any of the three drops to S0 at once
    {
        static const int bad[] = {PP_PMU_VBUS_GOOD | PP_PMU_BATT_PRESENT, 0, -1, PP_PMU_VBUS_GOOD};
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            for (int asleep = 0; asleep < 2; asleep++) {
                rig_init(&r, 0); to_s1(&r);
                if (asleep) { r.in.screen_asleep = true; run(&r, PP_SETTLE_MS + 200); assert(r.p.state == PP_S2); }
                r.status0 = bad[i];
                pp_state_t s = r.p.state;
                for (int k = 0; k < 25 && s != PP_S0; k++) s = step(&r, 100);
                assert(s == PP_S0 && r.last_action == PP_ACT_ENTER_S0);
            }
        }
    }
    // a stale status byte is not a reason to leave: only a fresh read counts
    rig_init(&r, 0); to_s2(&r);
    {
        pp_out_t out; r.in.now_ms += 100; r.in.chg_fresh = false; r.in.status0 = PP_PMU_VBUS_GOOD; pp_step(&r.p, &r.in, &out);
        assert(out.state == PP_S2 && out.action == PP_ACT_NONE);
    }

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
        {PP_POLLER_TOUCH, 20, 5000}, {PP_POLLER_RENDER, 1000, 5000}, {PP_POLLER_CABLE_READER, 100, 2000},
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
    // D1: no touch wake in S2, and every touch is dropped on a dark battery screen, so the touch fallback poll has nothing to catch: 5 s. (With
    // the level wake built in it is the 1 s of the plan.) The screen wake pokes the touch task so the long wait never delays a touch after a PWR wake.
    assert(PP_TOUCH_POLL_MS == 5000 && PP_TOUCH_WAKE_ON_BATTERY == 0);
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

    // --- D1 "PWR only, always" (plan section 16): on the battery a dark screen takes no input ---
    // The S2 PWR-key poll is a named constant (250 ms: the latched REG 0x49 status keeps a tap that lands between two polls).
    assert(PP_PWR_POLL_MS == 250 && PP_POLL_SLEEP_MS == PP_PWR_POLL_MS && pp_ptt_poll_ms(PP_S2) == PP_PWR_POLL_MS);
    // On external power (S0) touch and BOOT wake the screen as they always did, asleep or not.
    assert(pp_screen_input_allowed(PP_S0, true) && pp_screen_input_allowed(PP_S0, false));
    // On the battery (S1, S2) with the screen on, input works; with the screen off it is dropped (S1 too: the 3 s before S2).
    for (pp_state_t s = PP_S1; s <= PP_S2; s++) {
        assert(pp_screen_input_allowed(s, false));
        assert(!pp_screen_input_allowed(s, true));
    }
    // The touch level wake is not armed on the battery (D1); the arming code stays for a later mode.
    assert(PP_TOUCH_WAKE_ON_BATTERY == 0);

    // --- The PMIC power-off triggers (S3, plan section 3.6 / 16): 30 min in S2, a flat cell, a fast drain; never on external power ---
    test_poweroff_triggers();

    puts("power policy: S0/S1/S2 transitions, PMIC read schedule, hold bits, anti-flap, 32-bit clock wrap, poller table, battery screen levels, D1 input gate, power-off triggers PASS");
    return 0;
}
