// The glue between the pure mode switch (mode_switch.c) and the dial, as plain data and a table of actions (task W15). No IDF here: mode_glue.c
// reads the world into these structs and implements the actions with the real drivers; test_mode_glue.c runs the same code against a simulated dial.
//
// The contract from the W14 module, in the order a boot meets it:
//   mg_boot()  once, at least MS_SOF_SETTLE_MS after the reset, from the values mode_glue.c read (NVS, the SOF flag, the RTC record)
//   mg_init()  once, with the mode that really started (the radio may not have come up: then it is USB) and the flap count of the record
//   mg_step()  every MS_STEP_MS or faster
//   on REBOOT: log the reason, seal the record (focus and flaps), store boot_usb, THEN restart, in that order and nothing after
//   the USB probe is installed exactly while ms_out_t.probe_wanted, and uninstalled when it falls
//   when ms_out_t.flaps_cleared: re-seal the record at zero
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
} mg_boot_in_t;

typedef struct {
    ms_mode_t mode;             // MS_MODE_USB or MS_MODE_WIFI. Never BLE: no transport exists, so it would be a dial that cannot start.
    bool      write_boot_usb;   // store boot_usb_new in NVS (only when it differs from the stored value)
    bool      boot_usb_new;
    bool      sof_ignored;      // the SOF reading was too early to use (the glue waits, so this should never be true)
    bool      unsupported;      // wl_effective named a mode this firmware cannot start; the dial boots USB
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
    void (*restart)(void *ctx, ms_mode_t target);            // esp_restart()
} mg_ops_t;

typedef struct {
    ms_state_t ms;
    uint8_t    wl_effective;
    bool       stay_wl;
    bool       probe_on;
    bool       notice_on;
    bool       done;                 // the restart has been asked for: nothing more is done
} mg_t;

// `running` is the mode that really started, `flaps` the record's count, the last two the settings as read at boot.
void mg_init(mg_t *g, ms_mode_t running, uint8_t flaps, uint8_t wl_effective, bool stay_wl);
// The Settings screen (task W16) changed wl_mode or stay_wl: take the new values.
void mg_set_settings(mg_t *g, uint8_t wl_effective, bool stay_wl);
void mg_step(mg_t *g, const mg_world_t *w, const mg_ops_t *ops);
