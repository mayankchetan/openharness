#include "power_policy.h"

#include <string.h>

// (now - since) >= ms on a free-running 32-bit clock: right across the wrap as long as the span is under 49 days.
static bool span_reached(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

void pp_init(pp_t *p) { memset(p, 0, sizeof *p); p->state = PP_S0; }

// The three facts of a fresh read (power_policy.h). A failed REG 0x00 read (-1) is "not on the battery", like a failed direction read.
bool pp_on_battery(const pp_in_t *in)
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
    if (p->state != PP_S0 && (!in->capable || in->sof || (in->chg_fresh && !pp_on_battery(in)))) {
        to_s0(p, in, out);
        out->state = p->state;
        return;
    }

    switch (p->state) {
    case PP_S0:
        if (!in->capable || in->sof || in->chg != 2) {
            p->confirm_reads = 0;                       // the candidate went away: start again
        } else if (in->chg_fresh && !pp_on_battery(in)) {
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
    case PP_POLLER_NET_SERVE:     return 1000;
    case PP_POLLER_NET_BACKOFF:   return 2000;
    case PP_POLLER_MODE_SW:       return 2000;
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

// ---- the PMIC power-off triggers (power_policy.h) ----
void pp_off_init(pp_off_t *o) { memset(o, 0, sizeof *o); }

static void hist_reset(pp_off_t *o)
{
    o->hist_next = 0;
    o->hist_count = 0;
    o->have_sample = false;
    o->excursion = false;
}

int pp_off_drain_per_hour(const pp_off_t *o)
{
    if (o->hist_count < 2 || (o->hist_count - 1u) * PP_OFF_SAMPLE_MS < PP_OFF_DRAIN_MIN_MS) return -1;
    const unsigned first = o->hist_count < PP_OFF_SAMPLES ? 0u : o->hist_next;   // the oldest sample: slot 0, or, once the ring is full, the next to write
    const int oldest = o->hist[first];
    const int newest = o->hist[(o->hist_next + PP_OFF_SAMPLES - 1) % PP_OFF_SAMPLES];
    if (oldest <= newest) return 0;                // a gauge that rose (a cell that relaxed) is a drain of 0
    return (int)((unsigned)(oldest - newest) * 60u / (o->hist_count - 1u));   // percent over (count-1) minutes, per hour (rounded down)
}

pp_off_reason_t pp_off_step(pp_off_t *o, const pp_off_in_t *in)
{
    // External power in any form: no timers, no history, nothing to ask for.
    if (in->state == PP_S0 || !in->on_battery || in->sof) {
        o->s2_armed = false;
        o->low_armed = false;
        hist_reset(o);
        return PP_OFF_NONE;
    }
    // S1 with the screen on: the user is at the dial (a screen-on hour is not light-sleep drain). Everything starts again.
    const bool in_s2 = in->state == PP_S2;
    if (!in_s2 && !in->screen_asleep) {
        o->s2_armed = false;
        o->low_armed = false;
        hist_reset(o);
        return PP_OFF_NONE;
    }
    // "Dark" from here: S2, or S1 with the screen asleep (a hold excursion, or the 3 s before S2). Samples are taken in S2 only.
    if (!in_s2) {
        o->excursion = true;
    } else if (in->pct >= 0 && (!o->have_sample || span_reached(in->now_ms, o->last_sample_ms, PP_OFF_SAMPLE_MS))) {
        // One gauge sample a minute, in S2. A gap of two sample periods (the gauge unreadable for a while) throws the history away: it would
        // not be one run of minutes. A gap made only of dark S1 steps, no longer than PP_OFF_EXCURSION_MS, is forgiven.
        if (o->have_sample && span_reached(in->now_ms, o->last_sample_ms, 2 * PP_OFF_SAMPLE_MS) &&
            !(o->excursion && !span_reached(in->now_ms, o->last_sample_ms, PP_OFF_EXCURSION_MS)))
            hist_reset(o);
        o->hist[o->hist_next] = (uint8_t)(in->pct > 100 ? 100 : in->pct);
        o->hist_next = (uint8_t)((o->hist_next + 1) % PP_OFF_SAMPLES);
        if (o->hist_count < PP_OFF_SAMPLES) o->hist_count++;
        o->have_sample = true;
        o->last_sample_ms = in->now_ms;
        o->excursion = false;
    }
    if (!o->s2_armed) {
        o->s2_armed = true;
        o->s2_since_ms = in->now_ms;
    }
    if (in->pct >= 0 && in->pct <= PP_OFF_LOW_PCT) {
        if (!o->low_armed) {
            o->low_armed = true;
            o->low_since_ms = in->now_ms;
        }
    } else {
        o->low_armed = false;                                   // above 5 percent, or unknown: the minute starts again
    }
    if (o->low_armed && span_reached(in->now_ms, o->low_since_ms, PP_OFF_LOW_HOLD_MS)) return PP_OFF_LOW_BATTERY;
    if (pp_off_drain_per_hour(o) > PP_OFF_DRAIN_PCT_PER_H) return PP_OFF_DRAIN;
    if (span_reached(in->now_ms, o->s2_since_ms, PP_OFF_S2_MS)) return PP_OFF_S2_TIMEOUT;
    return PP_OFF_NONE;
}

const char *pp_off_reason_name(pp_off_reason_t r)
{
    switch (r) {
    case PP_OFF_NONE:       return "none";
    case PP_OFF_LOW_BATTERY: return "low_battery";
    case PP_OFF_DRAIN:      return "drain";
    case PP_OFF_S2_TIMEOUT: return "s2_30min";
    }
    return "?";
}
