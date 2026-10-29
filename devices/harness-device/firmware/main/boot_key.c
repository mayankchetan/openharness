#include "boot_key.h"

#include <string.h>

// (now - since) >= ms on a free-running 32-bit clock: right across the wrap as long as the span is under 49 days.
static bool span_reached(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

static void capture_forget(bk_t *k);

void bk_init(bk_t *k, bool pressed_now) { bk_init_ex(k, pressed_now, false); }

void bk_init_ex(bk_t *k, bool pressed_now, bool toggle_mode)
{
    memset(k, 0, sizeof *k);
    k->toggle_mode = toggle_mode;
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
        capture_forget(k);                               // a new press ends the wait for the old capture
        k->ignored = !allowed;
        return k->ignored ? BK_NONE : BK_PRESS;
    }
    if (pressed) {
        if (k->toggle_mode) {
            if (!k->ignored && !k->hold_fired && span_reached(now_ms, k->down_ms, BK_TOGGLE_MS)) {
                k->hold_fired = true;
                return BK_LONG;
            }
            return BK_NONE;
        }
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
    if (k->hold_fired && k->toggle_mode) {
        k->hold_fired = false;                           // the long press was the whole of it
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

void bk_capture_begin(bk_t *k, uint32_t token)
{
    k->has_token = true;
    k->token = token;
    k->rec_seen = false;
}

bool bk_capture_active(const bk_t *k) { return k->has_token; }

static void capture_forget(bk_t *k)
{
    k->has_token = false;
    k->rec_seen = false;
    k->stop_pending = false;
}

bk_cap_t bk_capture_poll(bk_t *k, uint32_t now_ms, bool recording, uint32_t token_now)
{
    if (!k->has_token) return BK_CAP_NONE;
    if (token_now != k->token) {                       // not the capture this hold started: leave it alone
        capture_forget(k);
        return BK_CAP_NONE;
    }
    if (recording && !k->rec_seen) {
        k->rec_seen = true;
        k->rec_began_ms = now_ms;
    }
    if (!k->stop_pending) return BK_CAP_NONE;          // the key is still down
    if (recording) {
        capture_forget(k);
        return span_reached(now_ms, k->rec_began_ms, BK_MIN_CLIP_MS) ? BK_CAP_SEND : BK_CAP_CANCEL;
    }
    if (k->rec_seen || span_reached(now_ms, k->stop_since_ms, BK_STOP_WAIT_MS)) capture_forget(k);   // it ended by itself, or it never began
    return BK_CAP_NONE;
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
