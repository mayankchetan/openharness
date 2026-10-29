#include "power_policy.h"

#include <string.h>

// (now - since) >= ms on a free-running 32-bit clock: right across the wrap as long as the span is under 49 days.
static bool span_reached(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

void pp_init(pp_t *p) { memset(p, 0, sizeof *p); p->state = PP_S0; }

// The three facts of a fresh read (power_policy.h). A failed REG 0x00 read (-1) is "not on the battery", like a failed direction read.
static bool on_battery(const pp_in_t *in)
{
    return in->chg == 2 && in->status0 >= 0 && !(in->status0 & PP_PMU_VBUS_GOOD) && (in->status0 & PP_PMU_BATT_PRESENT);
}

static bool dwell_ok(const pp_t *p, uint32_t now)
{
    return !p->dwell_armed || span_reached(now, p->dwell_since_ms, PP_DWELL_MS);
}

bool pp_wants_read(const pp_t *p, const pp_in_t *in)
{
    if (!in->capable) return false;
    if (p->state == PP_S0) {
        // Only a cached "discharging" is worth a bus read, and not while a host is there or the dwell runs: on a charger
        // (cache 0 or 1) the dial in S0 adds no I2C traffic at all.
        if (in->chg != 2 || in->sof || !dwell_ok(p, in->now_ms)) return false;
        return !p->have_read || span_reached(in->now_ms, p->last_read_ms, PP_CONFIRM_MS);
    }
    return !p->have_read || span_reached(in->now_ms, p->last_read_ms, PP_PMIC_READ_MS);
}

static void to_s0(pp_t *p, const pp_in_t *in, pp_out_t *out)
{
    p->state = PP_S0;
    p->confirm_reads = 0;
    p->asleep_armed = false;
    p->dwell_armed = true;
    p->dwell_since_ms = in->now_ms;
    out->action = PP_ACT_ENTER_S0;
}

void pp_step(pp_t *p, const pp_in_t *in, pp_out_t *out)
{
    out->from = p->state;
    out->action = PP_ACT_NONE;
    if (in->chg_fresh) {
        p->have_read = true;
        p->last_read_ms = in->now_ms;
    }

    // Anything unsafe leaves S1 and S2 at once. A stale value never does; a fresh one that is not "discharging" always
    // does, and so does a failed read (-1): when in doubt, do not sleep.
    if (p->state != PP_S0 && (!in->capable || in->sof || (in->chg_fresh && !on_battery(in)))) {
        to_s0(p, in, out);
        out->state = p->state;
        return;
    }

    switch (p->state) {
    case PP_S0:
        if (!in->capable || in->sof || in->chg != 2) {
            p->confirm_reads = 0;                       // the candidate went away: start again
        } else if (in->chg_fresh && !on_battery(in)) {
            p->confirm_reads = 0;                       // VBUS is good, or no cell, or a failed read: external power, start again
        } else if (in->chg_fresh && dwell_ok(p, in->now_ms)) {
            if (p->confirm_reads == 0) {
                p->confirm_reads = 1;
                p->confirm_first_ms = in->now_ms;
            } else if (span_reached(in->now_ms, p->confirm_first_ms, PP_CONFIRM_MS)) {
                p->state = PP_S1;
                p->confirm_reads = 0;
                p->asleep_armed = false;
                out->action = PP_ACT_ENTER_S1;
            }
        }
        break;
    case PP_S1:
        if (!in->screen_asleep) {
            p->asleep_armed = false;                    // a wake mid-settle cancels the count
        } else {
            if (!p->asleep_armed) {
                p->asleep_armed = true;
                p->asleep_since_ms = in->now_ms;
            }
            if (!in->holds && span_reached(in->now_ms, p->asleep_since_ms, PP_SETTLE_MS)) {
                p->state = PP_S2;
                p->s2_entries++;
                out->action = PP_ACT_ENTER_S2;
            }
        }
        break;
    case PP_S2:
        if (!in->screen_asleep || in->holds) {
            p->state = PP_S1;
            p->asleep_armed = false;
            out->action = PP_ACT_LEAVE_S2;
        }
        break;
    }
    out->state = p->state;
}

bool pp_screen_input_allowed(pp_state_t s, bool screen_asleep) { return s == PP_S0 || !screen_asleep; }

uint32_t pp_ptt_poll_ms(pp_state_t s) { return s == PP_S2 ? PP_POLL_SLEEP_MS : PP_POLL_AWAKE_MS; }

const char *pp_state_name(pp_state_t s)
{
    switch (s) {
    case PP_S0: return "S0";
    case PP_S1: return "S1";
    case PP_S2: return "S2";
    }
    return "S?";
}

// The relaxed waits in S2, one row per poller (plan section 3.3). The touch row is the fallback poll: the level wake on
// the controller's INT pin is the main path.
uint32_t pp_relaxed_ms(pp_poller_t who)
{
    switch (who) {
    case PP_POLLER_TOUCH:         return PP_TOUCH_POLL_MS;
    case PP_POLLER_RENDER:        return 5000;
    case PP_POLLER_CABLE_READER:  return 2000;
    case PP_POLLER_CABLE_SESSION: return 2000;
    case PP_POLLER_REFRESH:       return 5000;
    case PP_POLLER_BATLOG:        return 10000;
    case PP_POLLER_COUNT:         break;
    }
    return 0;
}

uint32_t pp_wait_ms(pp_state_t s, pp_poller_t who, uint32_t awake_ms)
{
    if (s != PP_S2 || who >= PP_POLLER_COUNT) return awake_ms;
    const uint32_t relaxed = pp_relaxed_ms(who);
    return awake_ms > relaxed ? awake_ms : relaxed;      // never shortens a wait
}

pp_screen_t pp_screen_level(pp_state_t s, uint32_t idle_ms)
{
    if (s == PP_S0) return idle_ms > PP_SCREEN_OFF_USB_MS ? PP_SCREEN_OFF : PP_SCREEN_FULL;
    if (idle_ms > PP_SCREEN_OFF_BATT_MS) return PP_SCREEN_OFF;
    if (idle_ms > PP_SCREEN_DIM_MS) return PP_SCREEN_DIM;
    return PP_SCREEN_FULL;
}

uint32_t pp_dimmed_percent(uint32_t user_percent)
{
    const uint32_t dimmed = user_percent * PP_DIM_PERCENT / 100u;
    return dimmed < PP_DIM_FLOOR ? PP_DIM_FLOOR : dimmed;
}
