// The unplugged mode "Wi-Fi first, else phone" as pure logic (wifi-first work, T1). Spec: workbench briefs/research/2026-11-11-wifi-first-phone-fallback-spec.md
// (rev 2, user-approved 2026-11-11), sections 3.2 to 3.4. Host-tested in test/test_link_auto.c.
//
// Nothing here calls the IDF, reads a pin, an NVS key, a task or a clock. The glue (mode_glue_core.c, with the facts read in mode_glue.c) passes the world
// in and gets back WHICH mode to reboot into and the RTC record to seal first. The decision to act is the mode switch's (mode_switch.c: the busy gate, the
// USB entry that has priority); this module only says "the other mode is due, and this is what the record says afterwards". It answers every step, so the
// answer is a LEVEL: while a reboot is held by the gate, or refused and retried, the same answer comes again and nothing is counted until the glue seals.
//
// Stored wl_mode 3 means auto. The mode switch never sees 3: mode_glue.c resolves it to the per-boot target (Wi-Fi or phone) before any mg_* call.
//
// The rules, in the order a boot meets them:
//   record     la_boot(): trusted only after a SOFTWARE reset with a good checksum and values in range. Any other reset ZEROES the record and seals it again
//              at once (the PMIC power-off keeps the RTC memory alive, so an old phone_boot must not be believed by a later software reset).
//   Wi-Fi boot works = the link is up AND the daemon's welcome came (the glue builds `up`). First true row wins:
//              works                          -> stay on Wi-Fi, the attempt rules stop for this boot
//              two scans without the SSID     -> phone at once (this beats the alert hold below: the access point is gone, so the daemon cannot be
//                                                reached either, and a held dial would sit without any link until a wake)
//              a TLS alert seen (sticky)      -> HOLD on Wi-Fi: no edge (the phone uses the same key)
//              LA_TRY_MS not works            -> phone
//              Wi-Fi lost after it worked for LA_WIFI_LOST_MS -> phone (not a retry). The clock does not run while the radio is parked on purpose
//              (S2: wifi off after 5 min dark) or waking from that park (la_in_t.hold): it starts when the hold ends and the link is still down.
//   phone boot  makes no Wi-Fi attempt. retries counts the returns to Wi-Fi (0-based, LA_RETRY_MAX of them per wake):
//              the session was up, then down for LA_DROP_WAIT_MS[retries]   -> Wi-Fi, retries + 1
//              the session never came for LA_NEVER_MS from the start        -> Wi-Fi, retries + 1
//              retries >= LA_RETRY_MAX is tested BEFORE the table is indexed.
//   bounds     3 retries per wake, LA_EDGE_CAP edges without a clear, LA_STABLE_MS of a stable session clears both. Worst case per wake: 1 + 2 x 3 = 7 reboots.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ── the numbers (spec 3.3, 3.4; Q1, Q2) ──────────────────────────────────────────────────────────────
#define LA_TRY_MS        30000u    // finding 2: one pass can take JOIN 15 s + TCP 5 s + TLS 5 s = 25 s; 30 s adds 20 percent
#define LA_NOAP_SCANS    2u        // scans in a row that find no access point (one missed scan at home must not cost a wake)
#define LA_WIFI_LOST_MS  120000u   // Q1: Wi-Fi lost in mid-session for this long goes to the phone
#define LA_NEVER_MS      120000u   // Q2: the phone never connected, this long after the phone stack started
#define LA_RETRY_MAX     3u        // returns to Wi-Fi per wake
#define LA_EDGE_CAP      8u        // automatic reboots without a clear, then no more (a belt: the rules above give 7)
#define LA_STABLE_MS     600000u   // a stable session this long clears retries and edges (the same 10 minutes as MS_FLAP_CLEAR_MS)
// The waits after a drop by retries used so far (0, 1, 2): the table has LA_RETRY_MAX entries and is read only through la_drop_wait_ms().
#define LA_DROP_WAIT_LIST 30000u, 60000u, 120000u

// ── the RTC record (12 bytes) ─────────────────────────────────────────────────────────────────────────
typedef struct {
    uint32_t magic;
    uint32_t check;       // CRC-32 over every field but this one
    uint8_t  phone_boot;  // 1: the boot this record belongs to starts on the phone
    uint8_t  retries;     // returns to Wi-Fi used so far (0-based), at most LA_RETRY_MAX
    uint8_t  edges;       // automatic reboots so far, at most LA_EDGE_CAP
    uint8_t  reserved;    // zero; in the checksum
} la_rec_t;

typedef struct {
    bool    phone_boot;
    uint8_t retries;
    uint8_t edges;
} la_view_t;

#define LA_REC_MAGIC 0x4C415243u   // "LARC"

uint32_t la_rec_crc(const la_rec_t *r);
// Fill a record. Values above their caps are held at the caps.
void la_seal(la_rec_t *r, bool phone_boot, uint8_t retries, uint8_t edges);
// At boot, once. `reset_is_software` is esp_reset_reason() == ESP_RST_SW. True and the contents when the record is trusted (software reset, magic and
// checksum right, phone_boot 0 or 1, retries <= LA_RETRY_MAX, edges <= LA_EDGE_CAP). Otherwise false, `view` (optional) is all zeros, and the record is
// ZEROED and sealed again, so a later software reset reads Wi-Fi. The record is left alone when trusted.
bool la_boot(la_rec_t *r, bool reset_is_software, la_view_t *view);

// ── the running state ─────────────────────────────────────────────────────────────────────────────────
typedef enum {
    LA_TARGET_NONE = 0,
    LA_TARGET_WIFI = 1,    // the numbers of ms_mode_t MS_MODE_WIFI and MS_MODE_BLE (checked in mode_glue_core.c)
    LA_TARGET_PHONE = 2,
} la_target_t;

typedef enum {
    LA_WHY_NONE = 0,
    LA_WHY_NO_AP,         // two scans without the SSID
    LA_WHY_WINDOW,        // not works by LA_TRY_MS
    LA_WHY_WIFI_LOST,     // lost after it worked, for LA_WIFI_LOST_MS
    LA_WHY_DROP,          // the phone session dropped, for the wait of this retry
    LA_WHY_NEVER,         // the phone session never came
    LA_WHY_COUNT
} la_why_t;

typedef struct {
    bool       active;        // a wireless mode runs and its rules are on
    bool       phone_boot;
    uint8_t    retries;
    uint8_t    edges;
    uint64_t   start_ms;      // the radio's start (Wi-Fi: net_link_start() returned; phone: the BLE start)
    uint64_t   last_now;      // a clock that steps back is held at this
    bool       up_ever;       // the session was up at some time in this boot
    bool       down_on;       // up before, and down now: the drop / lost clock runs
    uint64_t   down_since;
    bool       stable_on;
    uint64_t   stable_since;
    bool       alert_latched; // a TLS alert was seen in this Wi-Fi attempt (sticky)
    bool       said_works, said_alert, said_retries, said_edges;   // each event is reported once per episode
} la_t;

typedef struct {
    uint64_t now_ms;
    bool     up;           // Wi-Fi: net_state == NET_UP AND the daemon's welcome came; phone: the session is up
    uint8_t  noap_scans;   // Wi-Fi: scans in a row that found no access point (reset by a join)
    bool     alert_seen;   // Wi-Fi: the daemon answered a handshake with a TLS alert in this boot (net_link.c keeps it)
    bool     hold;         // Wi-Fi: the radio is parked on purpose (S2 wifi-off) or waking from the park (net_link_radio_waking): the link is down for a reason that is not a loss
} la_in_t;

#define LA_EV_WORKS           (1u << 0)   // Wi-Fi: the first step that was up; la_out_t.waited_ms = ms from the start
#define LA_EV_HOLD_ALERT      (1u << 1)   // Wi-Fi: an alert is held, no fallback
#define LA_EV_REFUSE_RETRIES  (1u << 2)   // phone: a return to Wi-Fi is due and LA_RETRY_MAX are spent
#define LA_EV_REFUSE_EDGES    (1u << 3)   // an edge is due and LA_EDGE_CAP are spent
#define LA_EV_STABLE          (1u << 4)   // LA_STABLE_MS of a stable session: retries and edges are zero now (la_out_t.now: seal it)

typedef struct {
    la_target_t target;     // LA_TARGET_NONE, or the mode that is due
    la_why_t    why;
    uint32_t    waited_ms;  // the time the trigger counted: since the start (window, no AP, never) or since the loss (drop, lost)
    la_view_t   next;       // with a target: the record for the boot after the reboot
    la_view_t   now;        // the record as it stands after this step (after a clear this is the one to seal)
    uint32_t    events;     // LA_EV_* bits, each reported once per episode
} la_out_t;

// `running` is the mode that really started (LA_TARGET_NONE for USB: the rules stay off). `view` is the record's contents (NULL: zeros).
void la_init(la_t *a, const la_view_t *view, la_target_t running, uint64_t start_ms);
void la_step(la_t *a, const la_in_t *in, la_out_t *out);

// The wait after a drop with `retries` used so far, or 0 when retries >= LA_RETRY_MAX (the table is not read).
uint32_t la_drop_wait_ms(uint8_t retries);
// An IDF Wi-Fi disconnect reason that means "no access point found" (WIFI_REASON_NO_AP_FOUND and the three _W_COMPATIBLE_SECURITY, _IN_AUTHMODE_THRESHOLD,
// _IN_RSSI_THRESHOLD reasons; wifi_sta.c asserts the numbers against the IDF enum).
bool la_noap_reason(int reason);
// The count of scans in a row that found no access point after one more Wi-Fi disconnect with `reason`: one more for a "no AP" reason (held at 255), zero
// for any other reason. wifi_sta.c keeps the count (event task only) and zeroes it at a join.
uint8_t la_noap_next(uint8_t count, int reason);
#define LA_REASON_NO_AP_FOUND                       201
#define LA_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210
#define LA_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211
#define LA_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD     212

const char *la_target_name(la_target_t t);   // "none", "wifi", "phone", "?"
const char *la_why_name(la_why_t w);         // for the log: "no_ap", "window over", "wifi lost", "drop", "never connected"
