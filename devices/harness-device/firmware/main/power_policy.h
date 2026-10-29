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
// The touch fallback poll in S2 (plan SLEEP_TOUCH_POLL_MS). With the level wake it backs up the interrupt (1 s); under D1 there is no wake and every
// contact on a dark battery screen is dropped, so it has nothing to catch and the touch task only needs to look up now and then (5 s). The screen
// wake pokes the task (touch_wake_task), so the long wait never delays a touch after a PWR wake.
#if PP_TOUCH_WAKE_ON_BATTERY
#define PP_TOUCH_POLL_MS 1000
#else
#define PP_TOUCH_POLL_MS 5000
#endif
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

// THE PMIC POWER-OFF TRIGGERS (S3, DEVICE_BATTERY_POWEROFF; plan sections 3.6 and 16). From S2 (battery, screen off) the dial asks to be
// switched off by the PMIC (power_off.c) when ANY of these holds:
//   (a) PP_OFF_S2_MS, 30 min, continuously in S2;
//   (b) the gauge reads PP_OFF_LOW_PCT percent or less, without a break, for PP_OFF_LOW_HOLD_MS (the minute keeps a gauge that has not
//       started yet, and reads 0, from being taken for a flat cell right after a boot);
//   (c) the gauge fell faster than PP_OFF_DRAIN_PCT_PER_H, measured on S2 samples only: one sample a minute while in S2, the history starts
//       again whenever S2 is left, and nothing is judged before PP_OFF_DRAIN_MIN_MS (20 min) of unbroken S2. The rate is the fall over
//       the minutes of history, scaled to an hour, so it catches a chip that does not sleep (what light sleep is for) and never the hours of
//       screen-on use that came before the S2. THE 15 PERCENT AN HOUR IS PROVISIONAL: the plan wants it set from the measured light-sleep
//       drain (T0/G6), which is not in yet. The 30 min of (a) ends an S2 that is not draining.
// NEVER on external power: a read that is not "on the battery" (VBUS good, no cell), a USB host (SOF), or S0 gives PP_OFF_NONE, throws the
// timers away and, for external power, the drain history too. power_off.c checks VBUS once more, fresh, before it acts.
#define PP_OFF_S2_MS            (30u * 60u * 1000u)
#define PP_OFF_LOW_PCT          5
#define PP_OFF_LOW_HOLD_MS      60000u
#define PP_OFF_DRAIN_PCT_PER_H  15          // PROVISIONAL, see above
#define PP_OFF_DRAIN_MIN_MS     (20u * 60u * 1000u)   // unbroken S2 history needed before the drain is judged
#define PP_OFF_SAMPLE_MS        60000u      // one gauge sample a minute, in S2
#define PP_OFF_SAMPLES          61          // the ring: up to an hour of S2 samples

typedef enum {
    PP_OFF_NONE = 0,
    PP_OFF_LOW_BATTERY = 1,   // (b)
    PP_OFF_DRAIN = 2,         // (c)
    PP_OFF_S2_TIMEOUT = 3,    // (a)
} pp_off_reason_t;

typedef struct {
    bool     s2_armed;
    uint32_t s2_since_ms;
    bool     low_armed;
    uint32_t low_since_ms;
    // the gauge history: a ring of the last PP_OFF_SAMPLES percents
    uint8_t  hist[PP_OFF_SAMPLES];
    uint8_t  hist_next;
    uint8_t  hist_count;
    bool     have_sample;
    uint32_t last_sample_ms;
} pp_off_t;

typedef struct {
    uint32_t   now_ms;
    pp_state_t state;       // the policy state after this poll's pp_step
    bool       on_battery;  // the last fresh PMIC read said so (pp_on_battery)
    bool       sof;         // a USB host is there
    int        pct;         // the gauge's own percent, 0..100, or -1
} pp_off_in_t;

void pp_off_init(pp_off_t *o);
// One step, every poll. Returns the reason the dial should be switched off for, or PP_OFF_NONE. A reason repeats on every step until it
// stops holding; the caller acts once (power_off_request refuses a second one while the first runs).
pp_off_reason_t pp_off_step(pp_off_t *o, const pp_off_in_t *in);
// The fall in percent, scaled to an hour, over the S2 history, or -1 while there is less than PP_OFF_DRAIN_MIN_MS of it (or it was thrown away).
int pp_off_drain_per_hour(const pp_off_t *o);
const char *pp_off_reason_name(pp_off_reason_t r);

// "On the battery", the three facts of one fresh read (see the rules at the top of this file).
bool pp_on_battery(const pp_in_t *in);
