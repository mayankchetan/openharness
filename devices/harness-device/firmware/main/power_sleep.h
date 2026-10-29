#pragma once
// BATTERY SLEEP, the ESP-IDF half (DEVICE_BATTERY_SLEEP; the decisions are power_policy.c). With the flag off this header is a set
// of inline no-ops and power_sleep.c is not built, so every call site stays unconditional and the image is the one it was.
#include <stdbool.h>
#include <stdint.h>

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

#else

static inline void power_sleep_init(void) {}

#endif
