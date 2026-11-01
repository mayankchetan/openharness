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
//     MS_CRASH_CLEAR_MS of uptime in Wi-Fi mode
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mode_switch.h"

// ── boot ────────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  wl_effective;      // config_load_wl_effective(): 1 = Wi-Fi is a usable target, 0 = none. Any other number is a mode with no transport.
    bool     stay_wl;           // config_load_stay_wl()
    bool     boot_usb;          // config_load_boot_usb()
    bool     sof;               // usb_serial_jtag_is_connected(), read no earlier than MS_SOF_SETTLE_MS after the reset
    uint32_t sof_read_ms;       // the uptime of that read
    bool     record_ok;         // mode_record_boot() trusted the record (a software reset, magic and checksum right)
    ms_record_view_t record;    // its contents (zeros when not trusted)
    bool     crash_trip;        // mode_record_crash_boot(): this boot follows the MS_CRASH_LIMIT-th abnormal reset in a row in Wi-Fi mode
    uint8_t  crashes;           // the crash count after this boot's reset
} mg_boot_in_t;

typedef struct {
    ms_mode_t mode;             // MS_MODE_USB or MS_MODE_WIFI. Never BLE: no transport exists, so it would be a dial that cannot start.
    bool      write_boot_usb;   // store boot_usb_new in NVS (only when it differs from the stored value)
    bool      boot_usb_new;
    bool      sof_ignored;      // the SOF reading was too early to use (the glue waits, so this should never be true)
    bool      unsupported;      // wl_effective named a mode this firmware cannot start; the dial boots USB
    bool      crash_usb;        // the panic loop took USB although the table said Wi-Fi: the glue says why ("Wi-Fi mode crashed N times; started in USB")
    uint8_t   crashes;          // the crash count to seal (the input's)
    uint8_t   flaps;            // from the record, 0 if untrusted
    char      focus[MS_FOCUS_MAX];   // the agent to put back on the carousel, "" for none
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
} mg_ops_t;

typedef struct {
    ms_state_t ms;
    uint8_t    wl_effective;
    bool       stay_wl;
    bool       probe_on;
    bool       notice_on;
    bool       done;                 // the restart has been asked for: nothing more is done (until a refused restart gives the dial back)
    uint8_t    crashes;              // the crash count; zero after MS_CRASH_CLEAR_MS in Wi-Fi mode
    uint8_t    refusals;             // refused restarts in a row for the switch that is due; zero when none is due or one went
    uint64_t   retry_at;             // uptime before which the sequence is not started again (valid while refusals > 0)
    bool       held_said;            // the hold of the switch that is due has been reported (ops->held) ...
    uint32_t   held_busy;            // ... with this busy mask,
    bool       held_asleep;          // ... and this asleep flag
    bool       wl_pending;           // a read said "no wireless target" while Wi-Fi runs; waiting for a second one (mg_apply_settings)
    uint64_t   wl_pending_at;        // uptime of that first read
} mg_t;

// A drop to "no target" while a wireless mode runs is the user's pick of USB, which reboots the dial at once. The value comes from NVS, and a read that
// lands inside a config_net_set_mode write (marker removed, commit, marker written back) says "no target" for a few milliseconds. So the drop is acted
// on only when a second read, at least MG_CONFIRM_MS after the first, says the same. 600 ms is longer than any one write window (two commits, tens
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
