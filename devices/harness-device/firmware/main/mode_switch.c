// The automatic mode switch as pure logic: see mode_switch.h. No IDF, no clock, no NVS; the caller passes everything in.
#include "mode_switch.h"

#include <string.h>

const char *ms_mode_name(ms_mode_t mode)
{
    switch (mode) {
    case MS_MODE_USB:  return "usb";
    case MS_MODE_WIFI: return "wifi";
    case MS_MODE_BLE:  return "ble";
    }
    return "?";
}

static bool mode_valid(ms_mode_t m) { return m == MS_MODE_USB || m == MS_MODE_WIFI || m == MS_MODE_BLE; }
static bool wl_valid(uint8_t wl) { return wl == MS_WL_WIFI || wl == MS_WL_BLE; }

// ── boot ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
void ms_boot_decide(const ms_boot_in_t *in, ms_boot_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->mode = MS_MODE_USB;
    if (!in) { out->boot_usb_new = true; return; }

    if (!wl_valid(in->wl_mode)) {
        out->mode = MS_MODE_USB;                              // row 1: no wireless target, and never leave USB
    } else if (in->stay_wl) {
        out->mode = (ms_mode_t)in->wl_mode;                   // row 2
    } else if (in->boot_usb) {
        if (in->sof && in->sof_read_ms < MS_SOF_SETTLE_MS) {  // too early: the flag starts true and says nothing yet
            out->sof_ignored = true;
            out->mode = (ms_mode_t)in->wl_mode;
        } else {
            out->mode = in->sof ? MS_MODE_USB : (ms_mode_t)in->wl_mode;   // row 3, else row 4
        }
    } else {
        out->mode = (ms_mode_t)in->wl_mode;                   // row 4
    }
    out->boot_usb_new = out->mode == MS_MODE_USB;
    out->write_boot_usb = out->boot_usb_new != in->boot_usb;
}

// ── gate ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
bool ms_blocked(uint32_t busy, bool asleep)
{
    if (busy & MS_BUSY_HARD) return true;   // an update or an upload in flight: never, asleep or not
    return busy != 0 && !asleep;            // anything else: wait while the user can see it; a sleeping dial switches silently
}

// ── state machine ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
void ms_init(ms_state_t *s, ms_mode_t running_mode, uint8_t flaps_from_record)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->mode = running_mode;
    s->flaps = flaps_from_record > MS_FLAP_CAP ? (uint8_t)MS_FLAP_CAP : flaps_from_record;
}

void ms_note_manual_change(ms_state_t *s)
{
    if (s) s->flaps = 0;
}

// Wireless: is a switch to USB due?
static bool step_wireless(ms_state_t *s, const ms_in_t *in, uint64_t now, ms_out_t *out)
{
    const bool probe = in->sof && !in->stay_wl;
    out->probe_wanted = probe;
    if (probe && !s->probe_on) s->probe_since = now;   // a new plug-in (or stay_wl just went off): welcomes from before it do not count
    s->probe_on = probe;
    if (!probe) {
        s->enter_on = false;
        return false;
    }
    // A welcome counts if it came after the probe started and is no more than 3 s old. Anything older than 3 s can never carry a 5 s window.
    const bool fresh = in->have_welcome && in->last_welcome_ms >= s->probe_since && in->last_welcome_ms <= now
                    && now - in->last_welcome_ms <= MS_WELCOME_AGE_MS;
    if (!fresh) {
        s->enter_on = false;
        return false;
    }
    if (!s->enter_on) {
        s->enter_on = true;
        s->enter_start = now;
    }
    return now - s->enter_start >= MS_T_ENTER_MS;
}

// USB: is a switch to the wireless mode due?
static bool step_usb(ms_state_t *s, const ms_in_t *in, uint64_t now)
{
    if (!wl_valid(in->wl_mode)) {       // nothing to leave to
        s->lost_on = false;
        return false;
    }
    if (in->stay_wl) {                  // stay_wl: leave at once, cable in or out, daemon or not (user decision 2026-10-31)
        s->lost_on = false;
        return true;
    }
    if (!in->sof) {
        if (!s->lost_on) {
            s->lost_on = true;
            s->lost_since = now;
        }
        return now - s->lost_since >= MS_T_LEAVE_MS;
    }
    s->lost_on = false;
    // SOF present: the daemon timer runs from its newest byte, or from boot if it never spoke. A live session is a byte right now.
    uint64_t ref = 0;
    if (in->session_up) ref = now;
    else if (in->have_daemon_byte) ref = in->last_daemon_byte_ms < now ? in->last_daemon_byte_ms : now;
    return now - ref >= MS_T_LEAVE_DAEMON_MS;
}

void ms_step(ms_state_t *s, const ms_in_t *in, ms_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->target = MS_MODE_USB;
    if (!s || !in) return;

    uint64_t now = in->now_ms;
    if (now < s->last_now) now = s->last_now;   // a clock that steps back neither fires nor rewinds a timer
    s->last_now = now;

    // 10 minutes of uptime in one mode clear the count of switches in a row.
    if (now >= MS_FLAP_CLEAR_MS && s->flaps) {
        s->flaps = 0;
        out->flaps_cleared = true;
    }
    out->flaps = s->flaps;

    if (!mode_valid(s->mode)) return;

    bool due;
    ms_mode_t target;
    if (s->mode == MS_MODE_USB) {
        due = step_usb(s, in, now);
        target = (ms_mode_t)in->wl_mode;
    } else {
        due = step_wireless(s, in, now, out);
        target = MS_MODE_USB;
    }
    if (!due) return;
    out->target = target;

    // The anti-flap guards hold the switch back silently: they are not something the user can act on.
    if (now < MS_T_DWELL_MS || s->flaps >= MS_FLAP_CAP) return;

    // The busy gate: a notice while the user can see it, nothing on a dark screen.
    if (ms_blocked(in->busy, in->asleep)) {
        out->waiting = true;
        out->act = in->asleep ? MS_ACT_NONE : MS_ACT_NOTICE;
        return;
    }
    out->act = MS_ACT_REBOOT;
    out->flaps = (uint8_t)(s->flaps + 1u);
    out->boot_usb = target == MS_MODE_USB;
}

// ── RTC record ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t ms_record_crc(const ms_record_t *r)
{
    if (!r) return 0;
    // every field but the checksum itself: magic, then flaps + reserved + focus
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const unsigned char *)&r->magic, sizeof r->magic);
    crc = crc32_update(crc, (const unsigned char *)&r->flaps, sizeof *r - offsetof(ms_record_t, flaps));
    return ~crc;
}

void ms_record_seal(ms_record_t *r, const char *focus, uint8_t flaps)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->magic = MS_RECORD_MAGIC;
    r->flaps = flaps > MS_FLAP_CAP ? (uint8_t)MS_FLAP_CAP : flaps;
    if (focus) {
        size_t n = 0;
        while (n < MS_FOCUS_MAX - 1 && focus[n]) n++;
        memcpy(r->focus, focus, n);
    }
    r->check = ms_record_crc(r);
}

bool ms_record_load(const ms_record_t *r, bool reset_is_software, ms_record_view_t *view)
{
    if (view) memset(view, 0, sizeof *view);
    if (!r || !reset_is_software) return false;
    if (r->magic != MS_RECORD_MAGIC || r->check != ms_record_crc(r)) return false;
    if (r->flaps > MS_FLAP_CAP) return false;
    if (!memchr(r->focus, '\0', sizeof r->focus)) return false;
    if (view) {
        view->flaps = r->flaps;
        memcpy(view->focus, r->focus, sizeof view->focus);
    }
    return true;
}

// ── the panic loop ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
uint32_t ms_crash_crc(const ms_crash_t *r)
{
    if (!r) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const unsigned char *)&r->magic, sizeof r->magic);
    crc = crc32_update(crc, (const unsigned char *)&r->crashes, sizeof *r - offsetof(ms_crash_t, crashes));
    return ~crc;
}

void ms_crash_seal(ms_crash_t *r, uint8_t crashes, ms_mode_t running)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->magic = MS_CRASH_MAGIC;
    r->crashes = crashes > MS_CRASH_MAX ? (uint8_t)MS_CRASH_MAX : crashes;
    r->mode = (uint8_t)running;
    r->check = ms_crash_crc(r);
}

void ms_crash_boot(const ms_crash_t *r, ms_reset_t cls, ms_crash_view_t *out)
{
    ms_crash_view_t local;
    ms_crash_view_t *v = out ? out : &local;
    memset(v, 0, sizeof *v);
    if (!r || cls == MS_RESET_POWERON) return;
    if (r->magic != MS_CRASH_MAGIC || r->check != ms_crash_crc(r)) return;
    if (r->crashes > MS_CRASH_MAX || !mode_valid((ms_mode_t)r->mode)) return;
    v->crashes = r->crashes;
    if (cls == MS_RESET_CRASH && r->mode == (uint8_t)MS_MODE_WIFI) {
        if (v->crashes < MS_CRASH_MAX) v->crashes++;
        v->trip = v->crashes >= MS_CRASH_LIMIT;
    }
}
