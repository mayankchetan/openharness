// AXP2101 PMIC (I2C 0x34, shared bus) — minimal read-only battery status for the status bar.
#pragma once

#include <stdbool.h>

// Lazily attach the AXP2101 to the shared I2C bus. Idempotent; safe to call from any task.
// Returns false if the device couldn't be added (then the getters report "unknown").
bool power_init(void);

// Battery charge percent 0..100, or -1 if unknown (no driver/battery/read error).
int power_battery_pct(void);

// Battery voltage in mV, or -1 if unknown (no driver, read error, a value outside 2,500..4,600 mV, ADC off). Reads the bus: one
// task at a slow rate (ptt.c, every BATTERY_POLL_MS). If the battery-voltage ADC channel is off it switches it on and answers -1
// this time.
int power_battery_mv(void);

// True while the battery is actively charging (drives the charge bolt). False if unknown.
bool power_is_charging(void);

// The AXP2101's battery activity: 0 standby (on external power, full or idle), 1 charging, 2 discharging (on battery).
// -1 when unknown (no PMIC, read error). Reads the bus: call it from one task at a slow rate (ptt.c polls every 30 s
// into battery.c; everything else reads that cache).
int power_charge_state(void);

// True while plugged into external power — actively charging OR full/standby. Use this (not
// power_is_charging) to decide "on the charger", so a full battery still counts as plugged in.
bool power_is_on_external(void);

// Drain a single short-press "tap" of button A (the PWR key, wired to the AXP2101 PWRON pin, not a GPIO),
// read over I2C. Returns true once per short press. A LONG press is handled by the AXP2101 itself as a
// hardware restart and is never seen here.
bool power_take_pwrkey_tap(void);
