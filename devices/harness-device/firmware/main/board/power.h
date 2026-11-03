// AXP2101 PMIC (I2C 0x34, shared bus) — minimal read-only battery status for the status bar.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// AXP2101 datasheet V1.4 p.31 (workbench briefs/research/2026-10-29-axp2101-registers.md):
//   REG 0x00 PMU status1: bit 5 = VBUS good (1 good), bit 3 = battery present (1 present), bits 7:6 reserved.
//   REG 0x01 PMU status2: bits 6:5 = battery current direction (00 standby, 01 charge, 10 discharge, 11 reserved), bit 7 reserved.
#define POWER_STATUS0_VBUS_GOOD     0x20
#define POWER_STATUS0_BATT_PRESENT  0x08
// The battery current direction out of a raw REG 0x01: 0 standby, 1 charging, 2 discharging, 3 reserved; -1 for a failed read (s < 0).
// Bit 7 is reserved and is masked off. Pure, so the host tests run it.
static inline int power_chg_from_status2(int s) { return s < 0 ? -1 : (s >> 5) & 0x03; }

// The rail registers, READ ONLY, for one boot log line (2026-11-02): REG 0x80 DC-DC enable, 0x81 DC-DC mode, 0x82..0x8F DC-DC voltages, 0x90 and 0x91 the LDO
// enables, 0x92..0x9A the LDO voltages (datasheet V1.4 pages 50-53; workbench briefs/research/2026-10-29-axp2101-registers.md). The map of which rail feeds what
// on this board is not known: this line is how the bench finds out what is on. No rail register is ever written by this firmware.
#define POWER_RAILS_FIRST 0x80
#define POWER_RAILS_LAST  0x9A
#define POWER_RAILS_COUNT (POWER_RAILS_LAST - POWER_RAILS_FIRST + 1)
// "AXP2101 rails (read only): 80=xx 81=xx ... 9a=xx", "--" for a register that did not read (regs[i] < 0). Pure; a small buffer is cut, never overrun.
static inline int power_rails_format(char *out, size_t cap, const int regs[POWER_RAILS_COUNT])
{
    if (!cap) return 0;
    int n = snprintf(out, cap, "AXP2101 rails (read only):");
    if (n < 0) { out[0] = 0; return 0; }
    if ((size_t)n >= cap) return (int)cap - 1;
    for (int i = 0; i < POWER_RAILS_COUNT; i++) {
        const int w = regs[i] < 0 ? snprintf(out + n, cap - (size_t)n, " %02x=--", POWER_RAILS_FIRST + i)
                                  : snprintf(out + n, cap - (size_t)n, " %02x=%02x", POWER_RAILS_FIRST + i, regs[i] & 0xFF);
        if (w < 0) break;
        if ((size_t)w >= cap - (size_t)n) return (int)cap - 1;
        n += w;
    }
    return n;
}
// Reads and logs them once (a board without the PMIC reads nothing). Called at boot.
void power_log_rails(void);

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

// One fresh read of REG 0x00 and REG 0x01 for the battery sleep policy (the ptt task, the same I2C path as everything above).
// *status0 and *status1 are the raw bytes, or -1 for a read that failed (either may be NULL). Returns the same as power_charge_state()
// for REG 0x01: 0 standby, 1 charging, 2 discharging, 3 reserved, -1 on a failed read.
int power_pmu_read(int *status0, int *status1);

// True while plugged into external power — actively charging OR full/standby. Use this (not
// power_is_charging) to decide "on the charger", so a full battery still counts as plugged in.
bool power_is_on_external(void);

// Drain a single short-press "tap" of button A (the PWR key, wired to the AXP2101 PWRON pin, not a GPIO),
// read over I2C. Returns true once per short press. A LONG press is handled by the AXP2101 itself as a
// hardware restart and is never seen here.
bool power_take_pwrkey_tap(void);

#ifdef DEVICE_BATTERY_POWEROFF
// PMIC power-off (power_off.c only): set REG 0x10 bit 0 "Soft PWROFF" (AXP2101 datasheet V1.4 p.32) by read-modify-write. Every rail but
// RTCLDO goes off; the dial comes back only on a PWR hold (power-on source is the PMIC's EFUSE setting, plan 16 bench gates P1-P3). True when
// the write was accepted; on success the chip is without power a few milliseconds later and this does not return in practice.
bool power_pmic_soft_off(void);
// One raw PMIC register, or -1 on a failed read (for the boot log of the power-on and power-off source status bytes).
int power_read_reg(uint8_t reg);
#endif
