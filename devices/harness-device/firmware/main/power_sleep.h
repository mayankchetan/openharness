#pragma once
// BATTERY SLEEP, the ESP-IDF half (DEVICE_BATTERY_SLEEP; the decisions are power_policy.c). With the flag off this header is a set
// of inline no-ops and power_sleep.c is not built, so every call site stays unconditional and the image is the one it was.
#include <stdbool.h>
#include <stdint.h>

#include "power_policy.h"

#ifdef DEVICE_BATTERY_SLEEP

// Create the "dial_awake" no-light-sleep lock and take it: nothing sleeps until the policy lets go. Call once, early in
// app_main (after board_detect()).
void power_sleep_init(void);

// The ptt task calls this every loop: it reads the inputs (battery state, USB host, screen, work in flight), steps the policy
// (power_policy.c) and writes the state it wants. It never calls a power-management function that needs more than a lock.
void power_sleep_poll(void);

// The render task calls this at the top of every loop. It applies what power_sleep_poll() decided: power management on and off
// for the battery, and the S2 steps (I2S clocks, the stall-guard timer, the relaxed pollers).
void power_sleep_render_service(void);

// Work in flight that keeps the chip awake (PP_HOLD_* in power_policy.h). The policy itself reads the voice turn, the firmware
// update and the cable session; a transport with traffic of its own (a later Wi-Fi or BLE mode) holds PP_HOLD_LINK here.
void power_sleep_hold(uint32_t bits);
void power_sleep_release(uint32_t bits);

// Battery entry and exit (the render task calls these; see power_sleep_render_service()). enter: power management with
// frequency scaling 240/80 MHz; with `light` also automatic light sleep, once the CPU/cache retention RAM (about 13.4 KB) is
// allocated. False when the retention RAM is missing: the dial then runs on frequency scaling only. leave: back to the fixed
// 240 MHz, which frees the retention RAM.
bool power_sleep_pm_enter_battery(bool light);
void power_sleep_pm_leave_battery(void);

// THE POLLERS (power_policy.h pp_poller_t). Each loop that wakes the chip asks for its wait here and passes the wait it has
// today: the same number comes back in S0 and S1, the relaxed one in S2. power_sleep_ptt_poll_ms() is the same for the ptt
// task's own delay. power_sleep_relaxed() is true while the dial is in S2 (the pollers run at their relaxed rates).
uint32_t power_sleep_wait_ms(pp_poller_t who, uint32_t awake_ms);
uint32_t power_sleep_ptt_poll_ms(uint32_t awake_ms);
bool power_sleep_relaxed(void);

// For the alive line (display_habitat.c, pm= slp= slp_ms=): the policy state the render task has applied ("S0", "S1", "S2"), the number
// of light sleeps since boot (entries that really slept 1 ms or more) and the milliseconds the chip spent in them. 0 and 0 when the
// exit callback is not built in.
const char *power_sleep_state_name(void);
uint32_t power_sleep_sleeps(void);
uint32_t power_sleep_slept_ms(void);

// THE BATTERY SCREEN POLICY (power_policy.h pp_screen_level): what the screen should be after `idle_ms` without a touch. On external
// power: full, then off after 300 s. On the battery (policy S1 or S2): full, dim after 30 s, off after 60 s.
pp_screen_t power_sleep_screen_level(uint32_t idle_ms);

// D1, "PWR only" (power_policy.h pp_screen_input_allowed): false while the dial runs on its battery (policy S1 or S2) and the screen is
// off. The touch task then drops the contact instead of waking the screen, and the ptt task ignores BOOT. True on external power and
// whenever the screen is on. Cheap and lock-free: any task may call it.
bool power_sleep_input_allowed(void);

#else

static inline void power_sleep_init(void) {}
// Without the flag the screen goes off after 300 s idle, as it always did (the same number as PP_SCREEN_OFF_USB_MS).
static inline pp_screen_t power_sleep_screen_level(uint32_t idle_ms) { return idle_ms > PP_SCREEN_OFF_USB_MS ? PP_SCREEN_OFF : PP_SCREEN_FULL; }
static inline void power_sleep_poll(void) {}
static inline void power_sleep_render_service(void) {}
static inline void power_sleep_hold(uint32_t bits) { (void)bits; }
static inline void power_sleep_release(uint32_t bits) { (void)bits; }
static inline uint32_t power_sleep_wait_ms(pp_poller_t who, uint32_t awake_ms) { (void)who; return awake_ms; }
static inline uint32_t power_sleep_ptt_poll_ms(uint32_t awake_ms) { return awake_ms; }
static inline bool power_sleep_relaxed(void) { return false; }
static inline bool power_sleep_input_allowed(void) { return true; }

#endif
