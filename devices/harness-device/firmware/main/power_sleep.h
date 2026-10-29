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

#else

static inline void power_sleep_init(void) {}
static inline uint32_t power_sleep_wait_ms(pp_poller_t who, uint32_t awake_ms) { (void)who; return awake_ms; }
static inline uint32_t power_sleep_ptt_poll_ms(uint32_t awake_ms) { return awake_ms; }
static inline bool power_sleep_relaxed(void) { return false; }

#endif
