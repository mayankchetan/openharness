#include "mode_glue_core.h"

#include <string.h>

_Static_assert((uint8_t)LA_TARGET_WIFI == (uint8_t)MS_MODE_WIFI && (uint8_t)LA_TARGET_PHONE == (uint8_t)MS_MODE_BLE,
               "link_auto.h's targets are the numbers of ms_mode_t: the mode switch takes them as its auto_target");

// ── the stored 3 ────────────────────────────────────────────────────────────────────────────────────
uint8_t mg_resolve_wl(uint8_t stored, mg_resolve_t when, bool pick_phone, ms_mode_t running, bool *auto_on)
{
    if (auto_on) *auto_on = false;
    if (stored != MG_WL_AUTO) return stored;   // 0, 1, 2 as they are; any other number is wl_known()'s to refuse
    if (auto_on) *auto_on = true;
    if (when == MG_RESOLVE_BOOT) return pick_phone ? (uint8_t)MS_WL_BLE : (uint8_t)MS_WL_WIFI;
    // a reload, or the start of the glue: the wireless mode that runs; when USB runs (or the radio did not come up) an unplug is a fresh wake: Wi-Fi first
    return ms_mode_is_wireless(running) ? (uint8_t)running : (uint8_t)MS_WL_WIFI;
}

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────
void mg_boot(const mg_boot_in_t *in, mg_boot_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->mode = MS_MODE_USB;
    out->boot_usb_new = true;
    if (!in) return;

    // The target the mode switch sees is Wi-Fi, Bluetooth or nothing. Any other number (a corrupt byte, a later build) has no transport in this
    // firmware: it must read as "no target", never as a mode that boots into a dead end. The NVS accessor already maps everything but a whole
    // configuration to 0 or a known mode; this keeps the glue safe whatever it is handed.
    uint8_t wl = in->wl_effective;
    if (wl != MS_WL_WIFI && wl != MS_WL_BLE && wl != MS_WL_UNSET) {
        out->unsupported = true;
        wl = MS_WL_UNSET;
    }

    ms_boot_in_t b = {
        .wl_mode = wl,
        .stay_wl = in->stay_wl,
        .boot_usb = in->boot_usb,
        .sof = in->sof,
        .sof_read_ms = in->sof_read_ms,
    };
    ms_boot_out_t r;
    ms_boot_decide(&b, &r);
    out->mode = ms_mode_is_wireless(r.mode) ? r.mode : MS_MODE_USB;
    out->write_boot_usb = r.write_boot_usb;
    out->boot_usb_new = r.boot_usb_new;
    out->sof_ignored = r.sof_ignored;
    out->crashes = in->crashes;
    out->auto_on = in->auto_on;
    out->la = in->la;

    // The panic loop: the third abnormal reset in a row that ended a wireless run, Wi-Fi or BLE (and every one after it), sends this boot to USB. It ranks above the
    // table (stay_wl, a stale boot_usb) and below nothing; it leaves the flap count alone, and it keeps boot_usb honest (boot_usb follows the
    // boot choice) so the boot after it reads the table as usual. It speaks only when it changed the answer.
    if (in->crash_trip && ms_mode_is_wireless(out->mode)) {
        out->crash_usb = true;
        out->mode = MS_MODE_USB;
        out->boot_usb_new = true;
        out->write_boot_usb = !in->boot_usb;
    }

    out->la_clear = out->mode == MS_MODE_USB;   // a USB boot clears the link_auto record: the next wake starts at Wi-Fi

    if (in->record_ok) {
        out->flaps = in->record.flaps;
        memcpy(out->focus, in->record.focus, sizeof out->focus);
        out->focus[sizeof out->focus - 1] = 0;
    }
}

// ── running ─────────────────────────────────────────────────────────────────────────────────────────
// The wireless targets the glue knows; the same two numbers ms_boot_decide() accepts. Anything else is "no target".
static uint8_t wl_known(uint8_t wl) { return wl == MS_WL_WIFI || wl == MS_WL_BLE ? wl : (uint8_t)MS_WL_UNSET; }
_Static_assert(MS_WL_WIFI == (uint8_t)MS_MODE_WIFI && MS_WL_BLE == (uint8_t)MS_MODE_BLE, "the stored wl_mode byte is also the target mode (mode_switch.h)");

void mg_init(mg_t *g, ms_mode_t running, uint8_t flaps, uint8_t wl_effective, bool stay_wl)
{
    if (!g) return;
    memset(g, 0, sizeof *g);
    ms_init(&g->ms, running, flaps);
    g->wl_effective = wl_known(wl_effective);
    g->stay_wl = stay_wl;
}

void mg_set_settings(mg_t *g, uint8_t wl_effective, bool stay_wl)
{
    if (!g) return;
    g->wl_effective = wl_known(wl_effective);
    g->stay_wl = stay_wl;
}

void mg_note_crashes(mg_t *g, uint8_t crashes)
{
    if (g) g->crashes = crashes;
}

static void notice(mg_t *g, const mg_ops_t *ops, bool on)
{
    if (g->notice_on == on) return;
    g->notice_on = on;
    if (ops->notice) ops->notice(ops->ctx, on);
}

void mg_step(mg_t *g, const mg_world_t *w, const mg_ops_t *ops)
{
    if (!g || !w || !ops || g->done) return;

    ms_in_t in = {
        .now_ms = w->now_ms,
        .wl_mode = g->wl_effective,
        .stay_wl = g->stay_wl,
        .sof = w->sof,
        .session_up = w->session_up,
        .have_daemon_byte = w->have_daemon_byte,
        .last_daemon_byte_ms = w->last_daemon_byte_ms,
        .have_welcome = w->have_welcome,
        .last_welcome_ms = w->last_welcome_ms,
        .busy = w->busy,
        .asleep = w->asleep,
        .idle_ms = w->idle_ms,
    };
    // "Wi-Fi first, else phone": while auto is on and a wireless mode runs, link_auto's rules say which mode is due. The answer is a level (the same
    // every step while it holds); the mode switch applies it (no dwell, no flap count, the busy gate, USB entry first) and nothing is counted until the
    // reboot sequence below seals the record.
    la_out_t lo;
    memset(&lo, 0, sizeof lo);
    if (g->auto_on && g->la.active && ms_mode_is_wireless(g->ms.mode)) {
        const la_in_t li = {.now_ms = w->now_ms, .up = w->link_up, .noap_scans = w->noap_scans, .alert_seen = w->alert_seen, .hold = w->link_hold};
        la_step(&g->la, &li, &lo);
        if (lo.events && ops->auto_event) ops->auto_event(ops->ctx, lo.events, &g->la, &lo, w->now_ms);
        // A stable session cleared the retries and the edges: the record says so (a restart that is no switch must not find the old counts).
        if ((lo.events & LA_EV_STABLE) && ops->la_seal) ops->la_seal(ops->ctx, &lo.now);
        in.auto_target = (uint8_t)lo.target;
    }
    ms_out_t out;
    ms_step(&g->ms, &in, &out);
    // An edge held by USB entry (welcomes are arriving): said once per episode.
    if (out.auto_held) {
        if (!g->auto_usb_said) {
            g->auto_usb_said = true;
            if (ops->auto_event) ops->auto_event(ops->ctx, MG_AEV_HELD_USB, &g->la, &lo, w->now_ms);
        }
    } else {
        g->auto_usb_said = false;
    }

    // The probe follows the module's answer exactly: up while it is wanted, down when it is not. Edge-triggered, so a driver is installed once.
    if (out.probe_wanted != g->probe_on) {
        g->probe_on = out.probe_wanted;
        if (ops->probe) ops->probe(ops->ctx, out.probe_wanted);
    }
    if (out.flaps_cleared && ops->clear_flaps) ops->clear_flaps(ops->ctx);
    // 10 minutes up in a wireless mode: it works, and the crashes before it were not "in a row" with the next one. USB time does not count.
    if (g->crashes && ms_mode_is_wireless(g->ms.mode) && w->now_ms >= MS_CRASH_CLEAR_MS) {
        g->crashes = 0;
        if (ops->clear_crashes) ops->clear_crashes(ops->ctx);
    }

    // A due switch held by the busy gate: said once when the hold starts and again when what holds it changes. A hold that ended is forgotten, so the
    // next one is said afresh.
    if (out.waiting) {
        if (!g->held_said || g->held_busy != w->busy || g->held_asleep != w->asleep) {
            g->held_said = true;
            g->held_busy = w->busy;
            g->held_asleep = w->asleep;
            if (ops->held) ops->held(ops->ctx, out.target, w->busy, w->asleep);
            if (out.auto_edge && ops->auto_event) ops->auto_event(ops->ctx, MG_AEV_HELD_BUSY, &g->la, &lo, w->now_ms);
        }
    } else {
        g->held_said = false;
    }
    if (out.act == MS_ACT_NOTICE) {
        notice(g, ops, true);
        return;
    }
    notice(g, ops, false);                 // the reason went (cable pulled, busy cleared) or the switch goes now
    if (out.act == MS_ACT_NONE && !out.waiting) {   // no switch is due any more: the next one starts a fresh ladder
        g->refusals = 0;
        g->retry_at = 0;
    }
    if (out.act != MS_ACT_REBOOT) return;
    if (g->refusals && w->now_ms < g->retry_at) return;   // a refusal is recent: not before the delay is over (review finding 1)

    // THE SEQUENCE. Log the reason (so the next boot's report is not read as a fault), seal the focus and the flap count (so the cap holds across
    // the reset), store boot_usb (so the next boot does not undo the switch), and only then restart. If the store fails the restart still goes
    // ahead: the next boot picks the old mode, the flap count in the record has already been raised, and the cap bounds the detour.
    // The restart op looks at the busy gate once more at the very end (the steps above take a display lock, an NVS commit and a log flush, and the
    // user can start a voice turn meanwhile). If it says no, nothing was restarted: put back what the sequence wrote and try again at a later step.
    //
    // An automatic edge adds one step: the link_auto record of the boot that is about to start (which radio, the retries and the edges used) is sealed
    // right after the focus and before boot_usb, so by the time the restart runs everything the next boot reads is in place. A refused restart puts the
    // old record back (after undo).
    g->done = true;
    if (ops->log_reason) ops->log_reason(ops->ctx, out.target);
    if (out.auto_edge && ops->auto_event) ops->auto_event(ops->ctx, MG_AEV_EDGE, &g->la, &lo, w->now_ms);
    if (ops->seal) ops->seal(ops->ctx, out.flaps);
    if (out.auto_edge && ops->la_seal) ops->la_seal(ops->ctx, &lo.next);
    // What NVS holds NOW, read before the store: a refusal puts back this, not the running mode's value (they differ after a radio fallback).
    const bool boot_usb_before = ops->load_boot_usb ? ops->load_boot_usb(ops->ctx) : g->ms.mode == MS_MODE_USB;
    if (ops->store_boot_usb) (void)ops->store_boot_usb(ops->ctx, out.boot_usb);
    if (ops->restart && !ops->restart(ops->ctx, out.target)) {
        if (ops->undo) ops->undo(ops->ctx, boot_usb_before, g->ms.flaps);
        if (out.auto_edge && ops->la_seal) ops->la_seal(ops->ctx, &lo.now);   // the record as it stood: a reset for another reason must not find the edge
        g->done = false;
        if (g->refusals < 250) g->refusals++;
        g->retry_at = w->now_ms + mg_retry_delay_ms(g->refusals);
    }
}

uint32_t mg_retry_delay_ms(uint8_t refusals)
{
    uint64_t d = MG_RETRY_BASE_MS;
    for (uint8_t n = 1; n < refusals && d < MG_RETRY_MAX_MS; n++) d *= 2;
    return d > MG_RETRY_MAX_MS ? MG_RETRY_MAX_MS : (uint32_t)d;
}

mg_restart_t mg_restart_try(const mg_gate_ops_t *g)
{
    if (!g || !g->frozen || !g->freeze || !g->thaw || !g->blocked || !g->restart) return MG_RESTART_FROZEN;
    if (g->frozen(g->ctx)) return MG_RESTART_FROZEN;
    if (!g->freeze(g->ctx)) return MG_RESTART_WRITER;
    if (g->blocked(g->ctx)) {
        g->thaw(g->ctx);
        return MG_RESTART_BUSY;
    }
    g->restart(g->ctx);
    return MG_RESTART_GONE;
}

void mg_apply_settings(mg_t *g, uint8_t wl_effective, bool stay_wl, uint64_t now_ms)
{
    if (!g) return;
    const uint8_t wl = wl_known(wl_effective);
    g->stay_wl = stay_wl;
    if (wl == MS_WL_UNSET && g->wl_effective != MS_WL_UNSET) {
        if (!g->wl_pending) {            // seen once: wait for the second read
            g->wl_pending = true;
            g->wl_pending_at = now_ms;
            return;
        }
        if (now_ms < g->wl_pending_at || now_ms - g->wl_pending_at < MG_CONFIRM_MS) return;   // too soon to be a second opinion (a clock that stepped back waits too)
        g->wl_pending = false;           // confirmed
        g->wl_effective = MS_WL_UNSET;
        return;
    }
    g->wl_pending = false;               // a wireless target again (either one), or no target both before and after: nothing is waiting
    g->wl_effective = wl;
}

// ── "Wi-Fi first, else phone" ────────────────────────────────────────────────────────────────────────
void mg_auto_start(mg_t *g, bool auto_on, const la_view_t *view, uint64_t start_ms)
{
    if (!g) return;
    g->auto_on = false;
    g->auto_usb_said = false;
    memset(&g->la, 0, sizeof g->la);
    if (!auto_on || !ms_mode_is_wireless(g->ms.mode)) return;   // USB runs (the radio did not come up): the rules stay off
    la_init(&g->la, view, g->ms.mode == MS_MODE_BLE ? LA_TARGET_PHONE : LA_TARGET_WIFI, start_ms);
    g->auto_on = g->la.active;
}

void mg_set_auto(mg_t *g, bool auto_on, uint64_t now_ms)
{
    if (!g || auto_on == g->auto_on) return;       // a reload that finds what it knew changes nothing: no count is lost
    if (!auto_on) {
        g->auto_on = false;
        g->la.active = false;
        return;
    }
    const la_view_t fresh = {0};
    mg_auto_start(g, true, &fresh, now_ms);        // fresh on the mode that runs; USB running: stays off
}

la_view_t mg_auto_view(const mg_t *g)
{
    la_view_t v = {0};
    if (g) {
        v.phone_boot = g->la.phone_boot;
        v.retries = g->la.retries;
        v.edges = g->la.edges;
    }
    return v;
}
