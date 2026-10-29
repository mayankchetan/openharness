#pragma once
// THE BATTERY SLEEP POLICY (DEVICE_BATTERY_SLEEP). Pure logic: no ESP-IDF include, no clock, no I/O. The glue
// (power_sleep.c) reads the inputs, calls pp_step() every poll of the ptt task and carries out the action it returns.
// test/test_power_policy.c compiles this file with a plain cc.
//
// Three states (briefs/research/2026-10-29-battery-sleep-plan.md section 3.1):
//   S0 EXTERNAL     the default. External power, a USB host, no PMIC, no battery, or an unknown reading. Nothing
//                   sleeps, no clock changes: the dial behaves as it did before this file existed.
//   S1 BATT_AWAKE   the PMIC said "discharging" on two fresh reads at least PP_CONFIRM_MS apart, no USB host, and
//                   the board can do it. Screen on (or off for less than PP_SETTLE_MS, or work in flight).
//   S2 BATT_SLEEP   S1, and the screen has been off for PP_SETTLE_MS, and nobody holds the chip awake. The chip may
//                   enter light sleep. The PWR key (polled every PP_PWR_POLL_MS) is the only way to wake it (D1).
// Deep sleep (S3, plan Stage 2) is not here.
//
// Rules, first match wins, in this order every step:
//   * Any state to S0 at once: no capable board, a USB host (SOF), or a FRESH read that is not "on the battery" (see below; a
//     failed read of either register counts: when in doubt, never sleep). A stale value (cache) never leaves S1 or S2, a fresh one always does.
//   * S0 to S1: two fresh "on the battery" reads PP_CONFIRM_MS apart, no host, and PP_DWELL_MS since the last drop to S0.
//   * ON THE BATTERY is three facts from one fresh read of the AXP2101 (datasheet V1.4 p.31): REG 0x00 bit 5 VBUS good = 0, REG 0x00
//     bit 3 battery present = 1, and REG 0x01 bits 6:5 battery current direction = 2 (discharge). A charger that is not charging
//     (VBUS good, direction discharge or standby) is external power; a dial with no cell is external power.
//   * S1 to S2: screen asleep for PP_SETTLE_MS without a break, and no hold bit.
//   * S2 to S1: the screen woke, or a hold bit was set.
#include <stdbool.h>
#include <stdint.h>

typedef enum { PP_S0 = 0, PP_S1 = 1, PP_S2 = 2 } pp_state_t;

typedef enum {
    PP_ACT_NONE = 0,
    PP_ACT_ENTER_S1,    // S0 -> S1: configure power management, keep the awake lock
    PP_ACT_ENTER_S2,    // S1 -> S2: relax the pollers, arm the touch wake, release the awake lock
    PP_ACT_LEAVE_S2,    // S2 -> S1: take the awake lock back, restore the pollers
    PP_ACT_ENTER_S0,    // S1 or S2 -> S0: plugged in (or anything unsafe): undo everything. out.from says which
} pp_action_t;

// Timing (ms).
#define PP_CONFIRM_MS    2000   // two fresh "discharging" reads this far apart before S1
#define PP_SETTLE_MS     3000   // screen off this long before S2 (the render task finishes the panel-off and the last frame)
#define PP_DWELL_MS      10000  // after a drop to S0, no S1 for this long (anti-flap)
#define PP_PMIC_READ_MS  2000   // a fresh PMIC read this often in S1 and S2 (plug-in detection)
#define PP_POLL_AWAKE_MS 100    // the ptt task's delay in S0 and S1 (today's POLL_MS)
#define PP_PWR_POLL_MS   250    // the PWR-key poll in S2: the only wake source on the battery (D1). The AXP2101 latches the key press
                                // (REG 0x49), so a tap that lands between two polls is still seen at the next one
#define PP_POLL_SLEEP_MS PP_PWR_POLL_MS   // the ptt task's delay in S2

// Hold bits: work in flight that keeps the chip awake. Whoever starts the work sets its bit, whoever ends it clears it.
#define PP_HOLD_VOICE    (1u << 0)   // a voice turn (audio_client_active())
#define PP_HOLD_FWUPDATE (1u << 1)   // a firmware update
#define PP_HOLD_SESSION  (1u << 2)   // a cable session is up (a host whose SOF flickers)
#define PP_HOLD_LINK     (1u << 3)   // a transport with traffic (reserved for Wi-Fi or BLE modes)

// AXP2101 REG 0x00 PMU status1 bits (datasheet V1.4 p.31); board/power.h has the same two numbers, test_power_policy.c checks they agree.
#define PP_PMU_VBUS_GOOD     0x20   // bit 5: 1 = VBUS good (external power is there)
#define PP_PMU_BATT_PRESENT  0x08   // bit 3: 1 = a battery is present

typedef struct {
    uint32_t now_ms;         // a free-running 32-bit millisecond clock; wrap is fine
    int      chg;            // the charge state known now: 0 standby, 1 charging, 2 discharging, -1 unknown (cache or fresh)
    bool     chg_fresh;      // true when chg (and status0) were read from the PMIC for this step
    int      status0;        // raw REG 0x00 of that fresh read, or -1 when it failed; ignored when chg_fresh is false
    bool     capable;        // board has the PMIC and the touch controller the wake path was built for
    bool     sof;            // a USB host is sending frames (usb_serial_jtag_is_connected())
    bool     screen_asleep;  // display_is_asleep()
    uint32_t holds;          // PP_HOLD_* bits
} pp_in_t;

typedef struct {
    pp_state_t state;
    // S0 -> S1 confirmation
    uint8_t    confirm_reads;       // consecutive fresh reads of 2 (0 or 1)
    uint32_t   confirm_first_ms;
    // PMIC read schedule
    bool       have_read;
    uint32_t   last_read_ms;
    // S1 -> S2
    bool       asleep_armed;
    uint32_t   asleep_since_ms;
    // anti-flap
    bool       dwell_armed;
    uint32_t   dwell_since_ms;
    // counters for the log
    uint32_t   s2_entries;
} pp_t;

typedef struct {
    pp_state_t  state;      // the state after this step
    pp_action_t action;     // what the glue must do now (PP_ACT_NONE for most steps)
    pp_state_t  from;       // the state before this step
} pp_out_t;

void pp_init(pp_t *p);
// One step. `in` is read, `p` advances, `out` says what changed.
void pp_step(pp_t *p, const pp_in_t *in, pp_out_t *out);
// True when the glue should read the PMIC now and pass the answer back with chg_fresh set. In S0 only a cached
// "discharging" is worth a read (so on a charger the dial adds no I2C traffic at all); in S1 and S2 every PP_PMIC_READ_MS.
bool pp_wants_read(const pp_t *p, const pp_in_t *in);

// The ptt task's delay.
uint32_t pp_ptt_poll_ms(pp_state_t s);
const char *pp_state_name(pp_state_t s);

// D1, "PWR only, always" (plan section 16, user decision 2026-10-29): on the battery, with the screen off, ONLY the PWR key wakes the
// dial. Touch never wakes it and touch events are dropped, BOOT does not wake it and is ignored. On external power (S0) nothing
// changes. "On the battery" is the policy state (S1 or S2), so the 3 s before S2 is covered as well.
bool pp_screen_input_allowed(pp_state_t s, bool screen_asleep);
// 1: arm GPIO11 (the touch controller's INT) as the light-sleep level wake in S2 (power_sleep.c, touch_habitat.c). 0 under D1: the
// arming code stays in the image's source for a later mode but is not called, so the chip sleeps through every touch.
#ifndef PP_TOUCH_WAKE_ON_BATTERY
#define PP_TOUCH_WAKE_ON_BATTERY 0
#endif

// THE POLLERS. Each loop that wakes the chip asks for its wait here, passing the wait it has today. S0 and S1 return
// that wait unchanged; S2 returns the relaxed one. A new poller is a new row in pp_relaxed_ms().
typedef enum {
    PP_POLLER_TOUCH,        // habitat_touch, idle wait (20 ms today). The 4 ms contact wait is not a poller: a contact wakes the screen.
    PP_POLLER_RENDER,       // habitat_render, wait ceiling when idle (1000 ms)
    PP_POLLER_CABLE_READER, // cable_link reader (100 ms)
    PP_POLLER_CABLE_SESSION,// cable_session (250 ms; hello only when a host is there)
    PP_POLLER_REFRESH,      // proj_refresh (1000 ms)
    PP_POLLER_BATLOG,       // battery log (1000 ms)
    PP_POLLER_COUNT
} pp_poller_t;
#define PP_TOUCH_POLL_MS 1000   // the touch fallback poll in S2 (plan SLEEP_TOUCH_POLL_MS); the level wake is the main path
uint32_t pp_relaxed_ms(pp_poller_t who);
uint32_t pp_wait_ms(pp_state_t s, pp_poller_t who, uint32_t awake_ms);

// THE SCREEN POLICY (plan T9, user decision 2026-10-29). On external power the screen goes off after PP_SCREEN_OFF_USB_MS
// without a touch, as it always did. On the battery (S1 or S2) it dims after PP_SCREEN_DIM_MS and goes off after
// PP_SCREEN_OFF_BATT_MS. "After" is strict: idle == the limit is still the earlier level.
#define PP_SCREEN_OFF_USB_MS  300000u
#define PP_SCREEN_DIM_MS      30000u
#define PP_SCREEN_OFF_BATT_MS 60000u
#define PP_DIM_PERCENT        25u   // dimmed brightness as a share of the user's setting
#define PP_DIM_FLOOR          3u    // ... and never below this many percent
typedef enum { PP_SCREEN_FULL = 0, PP_SCREEN_DIM, PP_SCREEN_OFF } pp_screen_t;
pp_screen_t pp_screen_level(pp_state_t s, uint32_t idle_ms);
uint32_t pp_dimmed_percent(uint32_t user_percent);
