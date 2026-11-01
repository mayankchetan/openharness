#include "mode_glue_core.h"

#include <string.h>

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────
void mg_boot(const mg_boot_in_t *in, mg_boot_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->mode = MS_MODE_USB;
    out->boot_usb_new = true;
    if (!in) return;

    // The target the mode switch sees is Wi-Fi or nothing. Bluetooth (2), and any number a corrupt byte or a later build could name, has no
    // transport in this firmware: it must read as "no target", never as a mode that boots into a dead end. The NVS accessor already maps
    // everything but a whole Wi-Fi configuration to 0; this keeps the glue safe whatever it is handed.
    uint8_t wl = in->wl_effective;
    if (wl != MS_WL_WIFI && wl != MS_WL_UNSET) {
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
    out->mode = r.mode == MS_MODE_WIFI ? MS_MODE_WIFI : MS_MODE_USB;
    out->write_boot_usb = r.write_boot_usb;
    out->boot_usb_new = r.boot_usb_new;
    out->sof_ignored = r.sof_ignored;
    out->crashes = in->crashes;

    // The panic loop: the third abnormal reset in a row that ended a Wi-Fi run (and every one after it) sends this boot to USB. It ranks above the
    // table (stay_wl, a stale boot_usb) and below nothing; it leaves the flap count alone, and it keeps boot_usb honest (boot_usb follows the
    // boot choice) so the boot after it reads the table as usual. It speaks only when it changed the answer.
    if (in->crash_trip && out->mode == MS_MODE_WIFI) {
        out->crash_usb = true;
        out->mode = MS_MODE_USB;
        out->boot_usb_new = true;
        out->write_boot_usb = !in->boot_usb;
    }

    if (in->record_ok) {
        out->flaps = in->record.flaps;
        memcpy(out->focus, in->record.focus, sizeof out->focus);
        out->focus[sizeof out->focus - 1] = 0;
    }
}

// ── running ─────────────────────────────────────────────────────────────────────────────────────────
void mg_init(mg_t *g, ms_mode_t running, uint8_t flaps, uint8_t wl_effective, bool stay_wl)
{
    if (!g) return;
    memset(g, 0, sizeof *g);
    ms_init(&g->ms, running, flaps);
    g->wl_effective = wl_effective == MS_WL_WIFI ? MS_WL_WIFI : MS_WL_UNSET;
    g->stay_wl = stay_wl;
}

void mg_set_settings(mg_t *g, uint8_t wl_effective, bool stay_wl)
{
    if (!g) return;
    g->wl_effective = wl_effective == MS_WL_WIFI ? MS_WL_WIFI : MS_WL_UNSET;
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
    };
    ms_out_t out;
    ms_step(&g->ms, &in, &out);

    // The probe follows the module's answer exactly: up while it is wanted, down when it is not. Edge-triggered, so a driver is installed once.
    if (out.probe_wanted != g->probe_on) {
        g->probe_on = out.probe_wanted;
        if (ops->probe) ops->probe(ops->ctx, out.probe_wanted);
    }
    if (out.flaps_cleared && ops->clear_flaps) ops->clear_flaps(ops->ctx);
    // 10 minutes up in Wi-Fi mode: it works, and the crashes before it were not "in a row" with the next one. USB time does not count.
    if (g->crashes && g->ms.mode == MS_MODE_WIFI && w->now_ms >= MS_CRASH_CLEAR_MS) {
        g->crashes = 0;
        if (ops->clear_crashes) ops->clear_crashes(ops->ctx);
    }

    if (out.act == MS_ACT_NOTICE) {
        notice(g, ops, true);
        return;
    }
    notice(g, ops, false);                 // the reason went (cable pulled, busy cleared) or the switch goes now
    if (out.act != MS_ACT_REBOOT) return;

    // THE SEQUENCE. Log the reason (so the next boot's report is not read as a fault), seal the focus and the flap count (so the cap holds across
    // the reset), store boot_usb (so the next boot does not undo the switch), and only then restart. If the store fails the restart still goes
    // ahead: the next boot picks the old mode, the flap count in the record has already been raised, and the cap bounds the detour.
    // The restart op looks at the busy gate once more at the very end (the steps above take a display lock, an NVS commit and a log flush, and the
    // user can start a voice turn meanwhile). If it says no, nothing was restarted: put back what the sequence wrote and try again at a later step.
    g->done = true;
    if (ops->log_reason) ops->log_reason(ops->ctx, out.target);
    if (ops->seal) ops->seal(ops->ctx, out.flaps);
    if (ops->store_boot_usb) (void)ops->store_boot_usb(ops->ctx, out.boot_usb);
    if (ops->restart && !ops->restart(ops->ctx, out.target)) {
        if (ops->undo) ops->undo(ops->ctx, g->ms.mode, g->ms.flaps);
        g->done = false;
    }
}
