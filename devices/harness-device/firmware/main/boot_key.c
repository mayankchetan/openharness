#include "boot_key.h"

#include <string.h>

// (now - since) >= ms on a free-running 32-bit clock: right across the wrap as long as the span is under 49 days.
static bool span_reached(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

void bk_init(bk_t *k, bool pressed_now)
{
    memset(k, 0, sizeof *k);
    if (pressed_now) {
        k->down = true;
        k->ignored = true;
    }
}

bk_event_t bk_step(bk_t *k, uint32_t now_ms, bool pressed, bool allowed, bool (*can_talk)(void))
{
    if (!k->down) {
        if (!pressed) return BK_NONE;
        k->down = true;                                  // the press edge
        k->down_ms = now_ms;
        k->hold_fired = false;
        k->talk_denied = false;
        k->stop_pending = false;                         // a new press ends the wait for the old capture
        k->ignored = !allowed;
        return k->ignored ? BK_NONE : BK_PRESS;
    }
    if (pressed) {
        if (!k->ignored && !k->hold_fired && !k->talk_denied && span_reached(now_ms, k->down_ms, BK_HOLD_MS)) {
            if (can_talk && can_talk()) {
                k->hold_fired = true;
                return BK_HOLD_START;
            }
            k->talk_denied = true;                       // asked once: this press ends as a tap
        }
        return BK_NONE;
    }
    k->down = false;                                     // the release
    if (k->ignored) {
        k->ignored = false;
        return BK_NONE;
    }
    if (k->hold_fired) {
        k->hold_fired = false;
        k->stop_pending = true;
        k->stop_since_ms = now_ms;
        return BK_HOLD_END;
    }
    return BK_TAP;
}

bool bk_stop_poll(bk_t *k, uint32_t now_ms, bool recording)
{
    if (!k->stop_pending) return false;
    if (recording) {
        k->stop_pending = false;
        return true;
    }
    if (span_reached(now_ms, k->stop_since_ms, BK_STOP_WAIT_MS)) k->stop_pending = false;   // it never began: nothing to stop
    return false;
}

bool bkr_begin(bkr_t *r, uint32_t now_ms, bool pressed)
{
    memset(r, 0, sizeof *r);
    r->last_secs = -1;
    if (!pressed) return false;
    r->started = true;
    r->since_ms = now_ms;
    return true;
}

bkr_out_t bkr_step(bkr_t *r, uint32_t now_ms, bool pressed)
{
    bkr_out_t o = {BKR_NONE, 0, false};
    if (!r->started) return o;
    if (r->done) { o.result = BKR_DONE; return o; }
    if (r->cancelled) { o.result = BKR_CANCELLED; return o; }
    if (!pressed) {                                      // the release counts before the clock does
        r->cancelled = true;
        o.result = BKR_CANCELLED;
        return o;
    }
    const uint32_t held = (uint32_t)(now_ms - r->since_ms);
    if (held >= BK_RESET_HOLD_MS) {
        r->done = true;
        o.result = BKR_DONE;
        return o;
    }
    o.result = BKR_COUNTING;
    o.seconds_left = (int)(BK_RESET_HOLD_MS / 1000u - held / 1000u);
    o.changed = o.seconds_left != r->last_secs;
    r->last_secs = o.seconds_left;
    return o;
}
