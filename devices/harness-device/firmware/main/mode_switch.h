// The automatic mode switch as pure logic (task W14). Three modes (USB, Wi-Fi, BLE); a switch is a reboot; the switch is automatic, with three
// guards (a daemon must answer before the dial enters USB, a busy dial waits, a flapping cable cannot loop the dial) and one manual override
// (stay_wl). The design is workbench briefs/research/2026-10-29-dial-mode-switch.md ("MS", rev 2) with the Wi-Fi spec's corrections
// (2026-10-29-wifi-mode-spec.md, section 8). Host-tested in test/test_mode_switch.c.
//
// Nothing here calls the IDF, reads a pin, an NVS key, a task or a clock. The glue (task W15: app_main.c, usb_probe.c, mode_record.c, the UI
// predicate) reads the world into a struct, calls one function and acts on the answer:
//
//   boot      ms_boot_decide()   once, 20 ms or more after the reset, after config_store_init: which mode to start in, and whether to rewrite boot_usb.
//   run       ms_step()          every MS_STEP_MS or faster, in the main loop or a 250 ms timer, in whichever mode runs: what to do now
//                                (nothing / show "switching in a moment" / reboot to a mode), whether the USB probe should be running, and the
//                                flap count to store.
//   record    ms_record_*()      the RTC record that carries the focus id and the flap count across the software reset.
//
// Time is milliseconds since boot (esp_timer_get_time() / 1000). It restarts at each boot, which is why the flap guard lives in the RTC record
// and not in a clock. Every timestamp in the inputs is on that same clock.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── numbers from MS section 1 (T_DWELL and the flap cap are proposals the user accepted on 2026-10-29) ────────────────────────────────────
#define MS_SOF_SETTLE_MS        20u       // the SOF flag starts TRUE in secondary init and falls only after ticks: read it no earlier than this
#define MS_T_ENTER_MS           5000u     // wireless -> USB: SOF continuous and welcomes coming for this long
#define MS_WELCOME_AGE_MS       3000u     // the newest welcome may be at most this old (the probe says hello every 2 s)
#define MS_T_LEAVE_MS           10000u    // USB -> wireless: SOF lost this long (not used with stay_wl: that leaves at once)
#define MS_T_LEAVE_DAEMON_MS    60000u    // USB -> wireless: SOF present, no byte from the daemon for this long (or since boot)
#define MS_T_DWELL_MS           60000u    // no automatic switch before this much uptime
#define MS_FLAP_CAP             3u        // automatic switches in a row, then no more until MS_FLAP_CLEAR_MS of uptime in one mode
#define MS_FLAP_CLEAR_MS        600000u   // 10 minutes in one mode clear the count
#define MS_STEP_MS              250u      // the busy gate is looked at this often (call ms_step at least this often)

#define MS_FOCUS_MAX            48        // the same size as ID_MAX in cable_client.h (agent id, uuid or 32 hex, plus the terminator)
#define MS_REASON_SWITCH        "mode switch"   // the last_words reason to log before the reboot, so the next report is not read as a fault

typedef enum {
    MS_MODE_USB = 0,
    MS_MODE_WIFI = 1,
    MS_MODE_BLE = 2,
} ms_mode_t;

// NVS wl_mode, the wireless mode the user picked in Settings. The numbers equal ms_mode_t so the stored byte is also the target mode. Anything
// else (including 0 and a corrupt byte) means "no wireless target": the dial boots USB and never leaves it.
#define MS_WL_UNSET             0u
#define MS_WL_WIFI              1u
#define MS_WL_BLE               2u

const char *ms_mode_name(ms_mode_t mode);   // "usb", "wifi", "ble", "?" for anything else

// ── boot decision (MS section 1 table, first match wins) ───────────────────────────────────────────────────────────────────────────────
//   1. wl_mode unset                          -> USB
//   2. stay_wl = 1                            -> wl_mode
//   3. boot_usb = 1 and SOF present (>= 20 ms) -> USB
//   4. otherwise                              -> wl_mode
// An SOF reading taken before MS_SOF_SETTLE_MS is ignored when it would decide row 3 (the flag starts true): the answer is then row 4 and
// sof_ignored says so. The glue should wait the 20 ms and read again rather than rely on that.
typedef struct {
    uint8_t  wl_mode;        // NVS wl_mode
    bool     stay_wl;        // NVS stay_wl
    bool     boot_usb;       // NVS boot_usb: 1 if the mode chosen at the last boot (or set by the last switch) was USB
    bool     sof;            // usb_serial_jtag_is_connected()
    uint32_t sof_read_ms;    // uptime when `sof` was read
} ms_boot_in_t;

typedef struct {
    ms_mode_t mode;          // the mode to start
    bool      write_boot_usb;   // store boot_usb_new in NVS (only when it differs from the stored value: one write per mode change)
    bool      boot_usb_new;     // mode == USB
    bool      sof_ignored;      // the reading was too early to use
} ms_boot_out_t;

void ms_boot_decide(const ms_boot_in_t *in, ms_boot_out_t *out);

// ── busy gate (MS section 3) ───────────────────────────────────────────────────────────────────────────────────────────────────────────
// The glue's mode_switch_blocked() builds this mask from the UI state; the module only applies it. The four rows of the user's list, the
// proposed rows, and two things that block even a sleeping dial.
#define MS_BUSY_VOICE           (1u << 0)   // voice turn: audio_client_active(), voice_open, voice_start_pending, voice_waiting, view VOICE
#define MS_BUSY_DRAFT           (1u << 1)   // draft.page.active, draft.pending, views DRAFT and DRAFT_OPTIONS
#define MS_BUSY_QUESTION        (1u << 2)   // views QUESTION, CHOICE, ANSWER_REVIEW with a valid question
#define MS_BUSY_READER          (1u << 3)   // view READER
#define MS_BUSY_PICKER          (1u << 4)   // views FORM, SELECTION, STOP, MODELS, MACHINES, TABS, OTA, MESSAGE
#define MS_BUSY_MODEL_LIST      (1u << 5)   // the blocking model list
#define MS_BUSY_FW_UPDATE       (1u << 6)   // fw_update_active(): blocks even asleep
#define MS_BUSY_PET_UPLOAD      (1u << 7)   // a pet pack arriving: blocks even asleep
#define MS_BUSY_HARD            (MS_BUSY_FW_UPDATE | MS_BUSY_PET_UPLOAD)

// True when a switch must wait. Any bit set blocks an awake dial (an unknown bit too: unsure means wait). Asleep, only MS_BUSY_HARD blocks.
bool ms_blocked(uint32_t busy, bool asleep);

// ── the running state machine ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Two modes of behaviour and no named sub-states; the timers below are the only memory.
//   wireless (Wi-Fi or BLE): SOF absent -> nothing, no probe. SOF present (and not stay_wl) -> the probe runs, hello every 2 s. The first welcome
//     that arrives after the probe started opens T_ENTER; SOF dropping, or the newest welcome older than 3 s, closes it. When T_ENTER has
//     lasted 5 s -> gate -> reboot to USB.
//   USB: stay_wl (with a valid wl_mode) -> gate -> reboot to wl_mode at once, cable in or out, live daemon or not (user decision 2026-10-31).
//     Otherwise: SOF absent for T_LEAVE (10 s), or SOF present with no daemon byte for T_LEAVE_DAEMON (60 s from the last byte, or from boot if
//     there was none; session_up counts as a byte now) -> gate -> reboot to wl_mode. Never when wl_mode is unset.
//   both: not before T_DWELL of uptime, not when MS_FLAP_CAP switches already ran in a row, and MS_FLAP_CLEAR_MS of uptime clears that count.
typedef struct {
    ms_mode_t mode;          // the running mode
    uint8_t   flaps;         // automatic switches in a row, from the RTC record at boot (0 if untrusted)
    uint64_t  last_now;      // the newest clock reading (a clock that steps back is held at this)
    bool      probe_on;      // wireless: probe asked for on the previous step
    uint64_t  probe_since;   // when it was asked for; welcomes older than this are from an earlier plug-in
    bool      enter_on;      // wireless: T_ENTER running
    uint64_t  enter_start;
    bool      lost_on;       // USB: SOF absent, T_LEAVE running
    uint64_t  lost_since;
} ms_state_t;

// Everything ms_step reads. The glue fills it fresh each call.
typedef struct {
    uint64_t now_ms;              // uptime
    uint8_t  wl_mode;             // NVS wl_mode, live (the Settings row may change it)
    bool     stay_wl;             // NVS stay_wl, live: USB leaves for wl_mode at once (after the dwell); wireless never probes
    bool     sof;                 // usb_serial_jtag_is_connected()
    // USB mode
    bool     session_up;          // cable_client_is_connected()
    bool     have_daemon_byte;    // at least one frame from the daemon since boot
    uint64_t last_daemon_byte_ms; // when the newest one arrived
    // wireless modes (from the probe)
    bool     have_welcome;        // the probe has parsed at least one welcome
    uint64_t last_welcome_ms;     // when the newest one arrived
    // gate
    uint32_t busy;                // MS_BUSY_* mask
    bool     asleep;              // display_is_asleep()
} ms_in_t;

typedef enum {
    MS_ACT_NONE = 0,
    MS_ACT_NOTICE,    // show "switching in a moment" (awake, the switch is due and a busy row holds it); call again within MS_STEP_MS
    MS_ACT_REBOOT,    // log MS_REASON_SWITCH, store the focus id and `flaps` (ms_record_seal), store `boot_usb`, esp_restart() into `target`
} ms_act_t;

typedef struct {
    ms_act_t  act;
    ms_mode_t target;         // the mode to boot into; set whenever a switch is due (NOTICE or REBOOT)
    bool      waiting;        // a switch is due and the gate holds it (asleep with an update or upload in flight: act stays NONE)
    bool      probe_wanted;   // wireless: install the USB driver and run the probe; false: uninstall it
    uint8_t   flaps;          // the flap count to keep: with REBOOT the new count (old + 1), otherwise the current one
    bool      flaps_cleared;  // the count has just gone to zero by 10 min of uptime: rewrite the RTC record
    bool      boot_usb;       // with REBOOT: the value of NVS boot_usb to store before restarting (target == USB). Without it the next boot
                              // finds the old value and picks the old mode again; storing it here makes the boot's own write unnecessary.
} ms_out_t;

void ms_init(ms_state_t *s, ms_mode_t running_mode, uint8_t flaps_from_record);
void ms_note_manual_change(ms_state_t *s);   // the user picked a mode by hand: the flap count restarts from zero
void ms_step(ms_state_t *s, const ms_in_t *in, ms_out_t *out);

// ── the RTC record (MS section 4; pattern of last_words.c) ─────────────────────────────────────────────────────────────────────────────────
// Lives in RTC_NOINIT memory in the glue (mode_record.c). Trusted only after a software reset (esp_reset_reason() == ESP_RST_SW); after a power
// cut, a watchdog or a panic the glue passes reset_is_software = false and gets zeros. CRC-32 over every field but the checksum.
typedef struct {
    uint32_t magic;
    uint32_t check;
    uint8_t  flaps;
    uint8_t  reserved[3];
    char     focus[MS_FOCUS_MAX];
} ms_record_t;

typedef struct {
    uint8_t flaps;
    char    focus[MS_FOCUS_MAX];   // empty: nothing to restore
} ms_record_view_t;

#define MS_RECORD_MAGIC 0x4D535743u   // "MSWC"

uint32_t ms_record_crc(const ms_record_t *r);
// Fill a record: focus is cut to MS_FOCUS_MAX - 1 characters (NULL = none), flaps is held at MS_FLAP_CAP.
void ms_record_seal(ms_record_t *r, const char *focus, uint8_t flaps);
// True and the contents if the reset was a software one, the magic and checksum match, flaps <= MS_FLAP_CAP and focus is terminated. Otherwise
// false and `view` (if given) is all zeros: a power cut gives zero, never garbage.
bool ms_record_load(const ms_record_t *r, bool reset_is_software, ms_record_view_t *view);

// ── the panic loop (user decision 2026-10-31) ──────────────────────────────────────────────────────────────────────────────────────────────
// A dial whose Wi-Fi mode crashes at every start would reboot at the speed of a panic and never be reachable by cable. A second RTC record counts
// the abnormal resets that ended a run in Wi-Fi mode. The boot that sees the MS_CRASH_LIMIT-th takes USB once (the boot decision, mode_glue_core.c)
// and logs why. The count is held by every reset that is not a crash (a software reset, so the switch back to Wi-Fi does not undo it), and cleared
// by two things only: a power-on, and MS_CRASH_CLEAR_MS of uptime in Wi-Fi mode (mode_glue_core.c). So after the first three crashes each further
// crash sends the next boot to USB at once, and a USB dial goes back to Wi-Fi no sooner than MS_T_DWELL_MS: a crash loop becomes one Wi-Fi attempt per
// dwell, not one per reboot. A power cut that loses the record loses the count (the safe side: the dial then boots as it always did).
//
// How a reset looks to this module. The glue maps esp_reset_reason() onto these three (mode_glue.c, reset_class(); the sets are fixed by
// test_mode_glue_source.py):
//   MS_RESET_POWERON  ESP_RST_POWERON
//   MS_RESET_HOLD     ESP_RST_SW, ESP_RST_DEEPSLEEP, and the resets somebody asked for: ESP_RST_EXT, ESP_RST_USB, ESP_RST_JTAG (a reset pin, a flash tool
//                     or a debugger; counting them would make three flashes in a row look like three crashes)
//   MS_RESET_CRASH    everything else: PANIC, INT_WDT, TASK_WDT, WDT, BROWNOUT, UNKNOWN, SDIO, EFUSE, PWR_GLITCH, CPU_LOCKUP, and any reason a later IDF adds
#define MS_CRASH_LIMIT      3u
#define MS_CRASH_CLEAR_MS   MS_FLAP_CLEAR_MS   // uptime in Wi-Fi mode that proves the mode works
#define MS_CRASH_MAX        250u               // the count saturates here; a byte never wraps round to "no crashes"

typedef enum {
    MS_RESET_POWERON = 0,   // zero on purpose: a record variable that was never set reads as a power-on
    MS_RESET_HOLD,
    MS_RESET_CRASH,
} ms_reset_t;

// In RTC_NOINIT memory (mode_record.c), believed after every reset but a power-on. CRC-32 over every field but the checksum.
typedef struct {
    uint32_t magic;
    uint32_t check;
    uint8_t  crashes;       // abnormal resets that ended a Wi-Fi run, held at MS_CRASH_MAX
    uint8_t  mode;          // the ms_mode_t that was running (or being started) when the record was last sealed
    uint8_t  reserved[2];
} ms_crash_t;

typedef struct {
    uint8_t crashes;        // the count after this boot's reset
    bool    trip;           // this boot is the MS_CRASH_LIMIT-th (or later) crash in a row: take USB once
} ms_crash_view_t;

#define MS_CRASH_MAGIC 0x4D534352u   // "MSCR"

uint32_t ms_crash_crc(const ms_crash_t *r);
// crashes is held at MS_CRASH_MAX.
void ms_crash_seal(ms_crash_t *r, uint8_t crashes, ms_mode_t running);
// What this boot's reset does to the count. A record that is not believed (magic, checksum, crashes <= MS_CRASH_MAX, mode one of the three) is a count
// of zero and counts nothing. POWERON: zero. HOLD (or a class outside the enum): the count as it was. CRASH: one more if the run that ended was Wi-Fi,
// else as it was. `trip` is set only by a CRASH that brings the count to MS_CRASH_LIMIT or more. Writes nothing.
void ms_crash_boot(const ms_crash_t *r, ms_reset_t cls, ms_crash_view_t *out);
