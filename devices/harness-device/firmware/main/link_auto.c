// "Wi-Fi first, else phone" as pure logic: see link_auto.h. No IDF, no clock, no NVS; the caller passes everything in.
#include "link_auto.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(la_rec_t) == 12, "the record is 12 bytes of RTC memory (spec section 2)");

static const uint32_t DROP_WAIT_MS[LA_RETRY_MAX] = {LA_DROP_WAIT_LIST};

uint32_t la_drop_wait_ms(uint8_t retries) { return retries < LA_RETRY_MAX ? DROP_WAIT_MS[retries] : 0u; }

bool la_noap_reason(int reason)
{
    return reason == LA_REASON_NO_AP_FOUND || reason == LA_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
        || reason == LA_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || reason == LA_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

uint8_t la_noap_next(uint8_t count, int reason)
{
    if (!la_noap_reason(reason)) return 0;
    return count < 255u ? (uint8_t)(count + 1u) : (uint8_t)255u;
}

const char *la_target_name(la_target_t t)
{
    switch (t) {
    case LA_TARGET_NONE:  return "none";
    case LA_TARGET_WIFI:  return "wifi";
    case LA_TARGET_PHONE: return "phone";
    }
    return "?";
}

const char *la_why_name(la_why_t w)
{
    switch (w) {
    case LA_WHY_NONE:      return "none";
    case LA_WHY_NO_AP:     return "no_ap";
    case LA_WHY_WINDOW:    return "window over";
    case LA_WHY_WIFI_LOST: return "wifi lost";
    case LA_WHY_DROP:      return "drop";
    case LA_WHY_NEVER:     return "never connected";
    case LA_WHY_COUNT:     break;
    }
    return "?";
}

// ── the record ───────────────────────────────────────────────────────────────────────────────────────
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t la_rec_crc(const la_rec_t *r)
{
    if (!r) return 0;
    uint32_t crc = 0xFFFFFFFFu;   // every field but the checksum: the magic, then phone_boot, retries, edges and the reserved byte
    crc = crc32_update(crc, (const unsigned char *)&r->magic, sizeof r->magic);
    crc = crc32_update(crc, (const unsigned char *)&r->phone_boot, sizeof *r - offsetof(la_rec_t, phone_boot));
    return ~crc;
}

void la_seal(la_rec_t *r, bool phone_boot, uint8_t retries, uint8_t edges)
{
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->magic = LA_REC_MAGIC;
    r->phone_boot = phone_boot ? 1u : 0u;
    r->retries = retries > LA_RETRY_MAX ? (uint8_t)LA_RETRY_MAX : retries;
    r->edges = edges > LA_EDGE_CAP ? (uint8_t)LA_EDGE_CAP : edges;
    r->check = la_rec_crc(r);
}

bool la_boot(la_rec_t *r, bool reset_is_software, la_view_t *view)
{
    if (view) memset(view, 0, sizeof *view);
    if (!r) return false;
    const bool ok = reset_is_software && r->magic == LA_REC_MAGIC && r->check == la_rec_crc(r) && r->phone_boot <= 1 && r->retries <= LA_RETRY_MAX
                 && r->edges <= LA_EDGE_CAP && r->reserved == 0;
    if (!ok) {
        la_seal(r, false, 0, 0);   // zeroed and sealed again at once: a later software reset reads Wi-Fi (finding 4)
        return false;
    }
    if (view) {
        view->phone_boot = r->phone_boot == 1;
        view->retries = r->retries;
        view->edges = r->edges;
    }
    return true;
}

// ── the running rules ────────────────────────────────────────────────────────────────────────────────
static la_view_t view_of(const la_t *a) { return (la_view_t){.phone_boot = a->phone_boot, .retries = a->retries, .edges = a->edges}; }

void la_init(la_t *a, const la_view_t *view, la_target_t running, uint64_t start_ms)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
    if (running != LA_TARGET_WIFI && running != LA_TARGET_PHONE) return;   // USB (or anything else): the rules stay off
    a->active = true;
    a->phone_boot = running == LA_TARGET_PHONE;   // what runs is the truth, whatever the record said
    if (view) {
        a->retries = view->retries;   // not clamped: la_step tests the cap before it reads the table
        a->edges = view->edges;
    }
    a->start_ms = start_ms;
    a->last_now = start_ms;
}

void la_step(la_t *a, const la_in_t *in, la_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->target = LA_TARGET_NONE;
    out->why = LA_WHY_NONE;
    if (!a) return;
    out->now = view_of(a);
    if (!in || !a->active) return;

    uint64_t now = in->now_ms;
    if (now < a->last_now) now = a->last_now;   // a clock that steps back neither fires nor rewinds a timer
    a->last_now = now;
    const uint64_t since_start = now - a->start_ms;

    // The session, and the clocks that follow it.
    if (in->up) {
        if (!a->up_ever && !a->phone_boot && !a->said_works) {
            a->said_works = true;
            out->events |= LA_EV_WORKS;
            out->waited_ms = (uint32_t)(since_start > 0xFFFFFFFFu ? 0xFFFFFFFFu : since_start);
        }
        a->up_ever = true;
        a->down_on = false;
        if (!a->stable_on) {
            a->stable_on = true;
            a->stable_since = now;
        }
        if ((a->retries || a->edges) && now - a->stable_since >= LA_STABLE_MS) {
            a->retries = 0;
            a->edges = 0;
            a->said_retries = a->said_edges = false;   // a new episode: its refusals are reported afresh
            out->events |= LA_EV_STABLE;
        }
    } else {
        a->stable_on = false;
        if (a->up_ever && !a->down_on) {
            a->down_on = true;
            a->down_since = now;
        }
    }

    la_target_t want = LA_TARGET_NONE;
    la_why_t why = LA_WHY_NONE;
    uint64_t waited = 0;
    bool retry = false;   // the edge is a return to Wi-Fi from the phone: counted in `retries`
    bool refuse_retries = false;

    if (!a->phone_boot) {
        if (a->up_ever) {
            // Wi-Fi lost after it worked. The attempt rules (window, scans, alert) are over for this boot.
            if (!in->up && a->down_on && now - a->down_since >= LA_WIFI_LOST_MS) {
                want = LA_TARGET_PHONE;
                why = LA_WHY_WIFI_LOST;
                waited = now - a->down_since;
            }
        } else {
            if (in->alert_seen) a->alert_latched = true;
            if (a->alert_latched) {
                // The daemon answered a handshake with an alert: it is reachable and it says no. The phone has the same key, so no fallback and no edge.
                if (!a->said_alert) {
                    a->said_alert = true;
                    out->events |= LA_EV_HOLD_ALERT;
                }
            } else if (in->noap_scans >= LA_NOAP_SCANS) {
                want = LA_TARGET_PHONE;
                why = LA_WHY_NO_AP;
                waited = since_start;
            } else if (since_start >= LA_TRY_MS) {
                want = LA_TARGET_PHONE;
                why = LA_WHY_WINDOW;
                waited = since_start;
            }
        }
    } else {
        if (a->up_ever) {
            if (!in->up && a->down_on) {
                // The guard is BEFORE the table: a count at the cap (or a corrupt one) never indexes it.
                if (a->retries >= LA_RETRY_MAX) {
                    refuse_retries = true;
                } else if (now - a->down_since >= la_drop_wait_ms(a->retries)) {
                    want = LA_TARGET_WIFI;
                    why = LA_WHY_DROP;
                    waited = now - a->down_since;
                    retry = true;
                }
            }
        } else if (since_start >= LA_NEVER_MS) {
            if (a->retries >= LA_RETRY_MAX) {
                refuse_retries = true;
            } else {
                want = LA_TARGET_WIFI;
                why = LA_WHY_NEVER;
                waited = since_start;
                retry = true;
            }
        }
    }

    if (refuse_retries && !a->said_retries) {
        a->said_retries = true;
        out->events |= LA_EV_REFUSE_RETRIES;
    }
    if (want != LA_TARGET_NONE && a->edges >= LA_EDGE_CAP) {
        if (!a->said_edges) {
            a->said_edges = true;
            out->events |= LA_EV_REFUSE_EDGES;
        }
        want = LA_TARGET_NONE;
        why = LA_WHY_NONE;
        waited = 0;
    }

    out->now = view_of(a);
    if (want == LA_TARGET_NONE) return;
    out->target = want;
    out->why = why;
    out->waited_ms = (uint32_t)(waited > 0xFFFFFFFFu ? 0xFFFFFFFFu : waited);
    out->next.phone_boot = want == LA_TARGET_PHONE;
    out->next.retries = (uint8_t)(a->retries + (retry ? 1u : 0u));
    out->next.edges = (uint8_t)(a->edges + 1u);
}
