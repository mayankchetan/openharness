// The glue between the pure mode switch (mode_switch.c) and the dial, as plain data and a table of actions (task W15). No IDF here: mode_glue.c
// reads the world into these structs and implements the actions with the real drivers; test_mode_glue.c runs the same code against a simulated dial.
//
// The contract from the W14 module, in the order a boot meets it:
//   mg_boot()  once, at least MS_SOF_SETTLE_MS after the reset, from the values mode_glue.c read (NVS, the SOF flag, the RTC record)
//   mg_init()  once, with the mode that really started (the radio may not have come up: then it is USB) and the flap count of the record
//   mg_step()  every MS_STEP_MS or faster
//   on REBOOT: log the reason, seal the record (focus and flaps), store boot_usb, THEN restart, in that order and nothing after. The restart op looks
//     at the busy gate once more right before it acts and may say no (false): the core then calls undo and tries again at a later step.
//   the USB probe is installed exactly while ms_out_t.probe_wanted, and uninstalled when it falls
//   when ms_out_t.flaps_cleared: re-seal the record at zero
//   the panic loop (mode_switch.h): the glue reads the crash record before mg_boot() and hands its answer in; mg_step() clears the count after
//     MS_CRASH_CLEAR_MS of uptime in a wireless mode (Wi-Fi or BLE: one count for both)
//   a BLE result (B8a): mg_boot() can now answer MS_MODE_BLE. Until the BLE link exists (B4b) app_main starts nothing for it and the dial runs USB in
//     that boot, exactly as when a radio does not come up; config_load_wl_effective() cannot return 2 before B6, so no dial reaches it yet.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_auto.h"
#include "mode_switch.h"

//   "Wi-Fi first, else phone" (wifi-first work; link_auto.h): the stored wl_mode 3 is NOT a target of this core. mode_glue.c resolves it to this boot's
//     target (1 or 2) with mg_resolve_wl() before any mg_* call, so wl_known() below stays "1, 2 or nothing". The core runs link_auto's rules while a
//     wireless mode runs and auto is on, hands the mode switch the mode that is due (ms_in_t.auto_target), and on an automatic edge seals the link_auto
//     record between the focus seal and the boot_usb store (ops->la_seal), putting the old record back when the restart is refused.
#define MG_WL_AUTO 3u            // the stored wl_mode byte for auto (config_store.h). Never a value of mg_t.wl_effective.

typedef enum { MG_RESOLVE_BOOT = 0, MG_RESOLVE_LIVE } mg_resolve_t;
// The one place the stored value becomes a target. `stored` is config_load_wl_effective(). For 0, 1, 2 (and anything else: wl_known filters it) it is returned
// as it is and *auto_on is false. For 3 (auto) *auto_on is true and the answer is Wi-Fi or the phone:
//   MG_RESOLVE_BOOT  this boot's pick (`pick_phone` from the link_auto record)
//   MG_RESOLVE_LIVE  a reload or the start of the glue: the wireless mode that runs, or Wi-Fi when USB runs (an unplug is a fresh wake)
// `auto_on` may be NULL.
uint8_t mg_resolve_wl(uint8_t stored, mg_resolve_t when, bool pick_phone, ms_mode_t running, bool *auto_on);

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  wl_effective;      // the target: 1 = Wi-Fi is a usable target, 2 = BLE is, 0 = none. Any other number is a mode with no transport. NEVER the stored 3: mg_resolve_wl() first.
    bool     stay_wl;           // config_load_stay_wl()
    bool     boot_usb;          // config_load_boot_usb()
    bool     sof;               // usb_serial_jtag_is_connected(), read no earlier than MS_SOF_SETTLE_MS after the reset
    uint32_t sof_read_ms;       // the uptime of that read
    bool     record_ok;         // mode_record_boot() trusted the record (a software reset, magic and checksum right)
    ms_record_view_t record;    // its contents (zeros when not trusted)
    bool     crash_trip;        // mode_record_crash_boot(): this boot follows the MS_CRASH_LIMIT-th abnormal reset in a row in a wireless mode
    uint8_t  crashes;           // the crash count after this boot's reset
    // after the fields above (a test written for the older struct still lines up)
    bool     auto_on;           // the stored wl_mode is 3 (auto): mg_resolve_wl() said so
    la_view_t la;               // the link_auto record's contents (zeros when not trusted): the pick that wl_effective was resolved from
} mg_boot_in_t;

typedef struct {
    ms_mode_t mode;             // MS_MODE_USB, MS_MODE_WIFI or MS_MODE_BLE. The glue starts a wireless mode only if its link comes up, else USB.
    bool      write_boot_usb;   // store boot_usb_new in NVS (only when it differs from the stored value)
    bool      boot_usb_new;
    bool      sof_ignored;      // the SOF reading was too early to use (the glue waits, so this should never be true)
    bool      unsupported;      // wl_effective named a mode this firmware cannot start (not 0, 1 or 2); the dial boots USB
    bool      crash_usb;        // the panic loop took USB although the table said wireless: the glue says why ("Wireless mode crashed N times; started in USB")
    uint8_t   crashes;          // the crash count to seal (the input's)
    uint8_t   flaps;            // from the record, 0 if untrusted
    char      focus[MS_FOCUS_MAX];   // the agent to put back on the carousel, "" for none
    bool      auto_on;          // the input's: the rules of link_auto.h run in a wireless mode that starts (mg_auto_start)
    la_view_t la;               // the input's record contents
    bool      la_clear;         // the result is USB: the link_auto record is zeroed (the glue does it: a USB boot clears it, a later wake starts at Wi-Fi)
} mg_boot_out_t;

void mg_boot(const mg_boot_in_t *in, mg_boot_out_t *out);

// ── running ─────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint64_t now_ms;
    bool     sof;
    bool     session_up;            // USB mode: cable_client_is_connected()
    bool     have_daemon_byte;      // USB mode: a frame has come from the daemon since boot
    uint64_t last_daemon_byte_ms;
    bool     have_welcome;          // wireless modes: the probe's newest welcome
    uint64_t last_welcome_ms;
    uint32_t busy;                  // MS_BUSY_*
    bool     asleep;
    uint32_t idle_ms;               // milliseconds since the last touch (display_idle_ms()): the wireless -> USB gate's idle rule
    // after the fields above. The link facts of link_auto.h, read only while auto is on and a wireless mode runs:
    bool     link_up;               // Wi-Fi: net_state == NET_UP AND the daemon's welcome came (cable_client_is_connected()); phone: the session is up
    uint8_t  noap_scans;            // Wi-Fi: scans in a row that found no access point
    bool     alert_seen;            // Wi-Fi: the daemon answered a handshake with a TLS alert in this boot
    bool     link_hold;             // Wi-Fi: the radio is parked on purpose (S2 wifi-off) or waking from the park: the lost-link clock does not run (link_auto.h)
} mg_world_t;

// What the glue can do. `ctx` goes to every call. All of them are called from the one task that calls mg_step.
typedef struct {
    void *ctx;
    void (*probe)(void *ctx, bool on);                       // install the USB driver and start the probe / take it down. Called only on a change.
    void (*notice)(void *ctx, bool on);                      // show / hide "switching in a moment". Called only on a change.
    void (*clear_flaps)(void *ctx);                          // ms_out_t.flaps_cleared: re-seal the RTC record at zero
    // the reboot sequence, in this order:
    void (*log_reason)(void *ctx, ms_mode_t target);         // last_words reason MS_REASON_SWITCH, and a log line
    void (*seal)(void *ctx, uint8_t flaps);                  // the focused agent and `flaps` into the RTC record
    bool (*store_boot_usb)(void *ctx, bool boot_usb);        // NVS boot_usb. A false return is logged by the glue and does not stop the restart.
    // esp_restart(), after one last look at the busy gate. False: the dial turned busy meanwhile and nothing was restarted (the core calls undo).
    // True is what a restart that returns at all says (a test double); the real one does not return.
    bool (*restart)(void *ctx, ms_mode_t target);
    // after the table above, so a test double written for the older table still lines up
    void (*clear_crashes)(void *ctx);                        // MS_CRASH_CLEAR_MS of uptime in Wi-Fi mode: re-seal the crash record at zero
    void (*undo)(void *ctx, bool boot_usb_before, uint8_t flaps);   // a refused restart: boot_usb back to `boot_usb_before`, the record back to `flaps`
    // after the table above. NVS boot_usb as it is NOW; the core reads it just before store_boot_usb and hands it to undo, so a refusal puts back what the
    // dial held (after a radio fallback that is the CHOICE, Wi-Fi, not the mode that runs). NULL: the running mode's value, as before W15e.
    bool (*load_boot_usb)(void *ctx);
    // after the table above. A due switch is held by the busy gate: called when it is first held and again each time the busy mask or the asleep flag
    // changes, once per hold (not on every step). The glue logs it: before this a replug that took five minutes left no line saying what held it.
    void (*held)(void *ctx, ms_mode_t target, uint32_t busy, bool asleep);
    // after the table above (wifi-first work). Both may be NULL (a table written before them).
    // la_seal: the link_auto record (RTC). Called in the reboot sequence of an AUTOMATIC edge right after `seal` and before `store_boot_usb`, with the record
    // of the boot that is about to start; again with the old record after a refused restart (after `undo`); and with the cleared counts when a stable session
    // clears them (ms_out stays untouched).
    void (*la_seal)(void *ctx, const la_view_t *record);
    // auto_event: something the glue should say (a log line, ESP_LOGW: over BLE only warnings reach the daemon). `ev` is a mask of LA_EV_* (link_auto.h) and
    // MG_AEV_*; `la` and `lo` give the numbers (`lo` is the answer of this step); `now_ms` the uptime.
    void (*auto_event)(void *ctx, uint32_t ev, const la_t *la, const la_out_t *lo, uint64_t now_ms);
} mg_ops_t;
// The core's own events, beside the LA_EV_* bits (1 << 0 to 1 << 4):
#define MG_AEV_EDGE      (1u << 8)    // an automatic edge is being taken now (the reboot sequence starts): lo->target, lo->why, lo->waited_ms, lo->next
#define MG_AEV_HELD_USB  (1u << 9)    // an edge is due and USB entry holds it: welcomes are arriving (once per episode)
#define MG_AEV_HELD_BUSY (1u << 10)   // an edge is due and the busy gate holds it (once per hold)

typedef struct {
    ms_state_t ms;
    uint8_t    wl_effective;
    bool       stay_wl;
    bool       probe_on;
    bool       notice_on;
    bool       done;                 // the restart has been asked for: nothing more is done (until a refused restart gives the dial back)
    uint8_t    crashes;              // the crash count; zero after MS_CRASH_CLEAR_MS in a wireless mode
    uint8_t    refusals;             // refused restarts in a row for the switch that is due; zero when none is due or one went
    uint64_t   retry_at;             // uptime before which the sequence is not started again (valid while refusals > 0)
    bool       held_said;            // the hold of the switch that is due has been reported (ops->held) ...
    uint32_t   held_busy;            // ... with this busy mask,
    bool       held_asleep;          // ... and this asleep flag
    bool       wl_pending;           // a read said "no wireless target" while a wireless mode runs; waiting for a second one (mg_apply_settings)
    uint64_t   wl_pending_at;        // uptime of that first read
    // wifi-first work
    bool       auto_on;              // the stored wl_mode is 3 and a wireless mode runs: link_auto's rules (`la`) are on
    la_t       la;
    bool       auto_usb_said;        // MG_AEV_HELD_USB has been reported for this episode
} mg_t;

// A drop to "no target" while a wireless mode runs is the user's pick of USB, which reboots the dial at once. The value comes from NVS, and a read that
// lands inside a config_net_set_mode write (marker removed, commit, marker written back) says "no target" for a few milliseconds. So the drop is acted
// on only when a second read, at least MG_CONFIRM_MS after the first, says the same. (A change from one wireless target to the other is a pick of a
// mode, not a lost read: applied at once.) 600 ms is longer than any one write window (two commits, tens
// of milliseconds) and two steps of the 250 ms task; the cost to the user is that much delay on a pick that takes a reboot anyway.
#define MG_CONFIRM_MS 600u

// After a refused restart the sequence (a boot_usb commit, an undo commit, a probe stop and start) is not run again at once. The delay doubles from
// MG_RETRY_BASE_MS to MG_RETRY_MAX_MS. Why these numbers: a refusal at the last moment means the user has just started something, or an NVS writer is
// stuck. 10 s is short enough that a switch the user is waiting for is not noticeably late (the notice is already gone while it waits), and the
// doubling to 120 s caps a stuck writer at about 35 attempts an hour (two flash commits each) instead of the 3,600 a one-second retry made.
#define MG_RETRY_BASE_MS 10000u
#define MG_RETRY_MAX_MS  120000u
uint32_t mg_retry_delay_ms(uint8_t refusals);   // the delay after the `refusals`-th refusal in a row (0 and 1: the base)

// The restart op's gate sequence, as data, so that a host test can run it (the real ops are in mode_glue.c). The order, which nothing may change:
//   frozen on entry?  -> MG_RESTART_FROZEN: the store gate is frozen by someone else (power_off.c, before the PMIC cuts the power). The switch is
//                        skipped and the gate is left as found: a restart would race the power-off, and a thaw would let writers into it.
//   freeze the writers -> on failure MG_RESTART_WRITER (the gate reopened itself; nothing to thaw)
//   the last look at the busy gate -> busy: thaw (this call froze it), MG_RESTART_BUSY
//   restart() -> MG_RESTART_GONE (the real one does not return)
typedef enum { MG_RESTART_GONE = 0, MG_RESTART_FROZEN, MG_RESTART_WRITER, MG_RESTART_BUSY } mg_restart_t;
typedef struct {
    void *ctx;
    bool (*frozen)(void *ctx);   // store_gate_frozen()
    bool (*freeze)(void *ctx);   // store_gate_freeze(RESTART_FREEZE_MS): true = no writer runs and none can start
    void (*thaw)(void *ctx);     // store_gate_thaw()
    bool (*blocked)(void *ctx);  // mode_switch_blocked()
    void (*restart)(void *ctx);  // esp_restart()
} mg_gate_ops_t;
mg_restart_t mg_restart_try(const mg_gate_ops_t *g);   // missing ops: MG_RESTART_FROZEN, nothing is called

// `running` is the mode that really started, `flaps` the record's count, the last two the settings as read at boot.
void mg_init(mg_t *g, ms_mode_t running, uint8_t flaps, uint8_t wl_effective, bool stay_wl);
// The Settings screen (task W16) changed wl_mode or stay_wl: take the new values.
void mg_set_settings(mg_t *g, uint8_t wl_effective, bool stay_wl);
// What the glue task calls on a reload (mode_glue_settings_changed) and again at every loop while `wl_pending` is set: like mg_set_settings, but a drop
// from Wi-Fi to "no target" is held until a second read at least MG_CONFIRM_MS later agrees (a read that says Wi-Fi again cancels it). stay_wl and a
// change to Wi-Fi are applied at once. `now_ms` is the uptime of the read.
void mg_apply_settings(mg_t *g, uint8_t wl_effective, bool stay_wl, uint64_t now_ms);
// After mg_init(): the crash count this boot started with (mg_boot_out_t.crashes).
void mg_note_crashes(mg_t *g, uint8_t crashes);
void mg_step(mg_t *g, const mg_world_t *w, const mg_ops_t *ops);

// ── "Wi-Fi first, else phone" (wifi-first work) ─────────────────────────────────────────────────────
// After mg_init(), once: switch the rules on when the stored mode is 3 (auto) and a wireless mode really runs. `view` is the record's contents (the pick
// this boot was resolved from, and the retries and edges used so far); `start_ms` the uptime of the radio's start. USB running (the radio did not come
// up) leaves the rules off.
void mg_auto_start(mg_t *g, bool auto_on, const la_view_t *view, uint64_t start_ms);
// A reload read a wireless target (the caller does not call this for a read that says "no target"): auto went on or off in NVS. Going on starts the
// rules fresh on the mode that runs (no retries, no edges used, the clock from `now_ms`); going off stops them. Nothing changes when `auto_on` is what
// it was, so a reload every few seconds loses no count.
void mg_set_auto(mg_t *g, bool auto_on, uint64_t now_ms);
// The record as it stands (what the glue seals when auto goes on).
la_view_t mg_auto_view(const mg_t *g);
