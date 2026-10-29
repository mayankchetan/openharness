#pragma once
// PMIC POWER-OFF (S3), DEVICE_BATTERY_POWEROFF (default OFF; needs DEVICE_BATTERY_SLEEP). Plan sections 3.6, 16 and 16.1 of
// briefs/research/2026-10-29-battery-sleep-plan.md in the workbench repo.
//
// The AXP2101 can switch itself off: REG 0x10 bit 0 "Soft PWROFF" (datasheet V1.4 p.32). Every rail but RTCLDO goes off (p.19, p.21), the
// battery then supplies typically 40 uA (Table 5-5, p.11), and the dial comes back only on a PWR hold; it is a full boot, the state comes
// from NVS. The triggers are the policy's (power_policy.h pp_off_step): 30 min in S2, a flat cell, a fast drain. The sequence is the
// reviewed one (16.1 item 2):
//   one log line -> stop the NVS writers (store_gate.h) -> write the record (reason, percent, mV) and commit it -> let the flash settle
//   -> "Off - hold PWR" on the glass for 1 s -> REG 0x10 bit 0.
// NEVER on external power: a fresh read of REG 0x00 must say VBUS is not good, and no USB host may be there. The bench trigger (the cable
// command debug.power.off, cable_debug.c) skips those two checks (force) so gates P1-P3 can run on a cable; it never skips the firmware-update check.
//
// With the flag off this header is empty inlines and power_off.c is not built.
#include <stdbool.h>

#define POWER_OFF_REASON_DEBUG 9   // the bench command; the policy's reasons are pp_off_reason_t (power_policy.h)

#ifdef DEVICE_BATTERY_POWEROFF
// Start the power-off task (internal stack: it writes NVS). False when one is already running, or, for an unforced request, within a minute
// of the last refusal. The task may still refuse (VBUS, USB host, firmware update, a writer that does not finish) and then leaves the dial as it
// was. `force`: skip the VBUS and USB-host checks (bench trigger only).
bool power_off_request(int reason, bool force);
bool power_off_pending(void);
// Boot, after NVS is up: log the AXP2101 power-on and power-off source bytes (REG 0x20, 0x21) and, if the last boot ended in a power-off,
// its record once, then erase it.
void power_off_boot_report(void);
#else
static inline void power_off_boot_report(void) {}
#endif
