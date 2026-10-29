#pragma once
// The battery figures the rest of the firmware reads: the alive line (bat=, chg=) and the hello frame (battery,
// charging). The PMIC is read in ONE place, the ptt task's loop (ptt.c, PMIC boards only), every BATTERY_POLL_MS; these
// are the cached results, so a reader on any other task never touches the I2C bus.
//
// charge state, as the AXP2101 reports it (board/power.c power_charge_state): 0 standby (full or idle, on external
// power), 1 charging, 2 discharging (on battery). -1 unknown.
#include <stdbool.h>
#include <stdint.h>

#define BATTERY_POLL_MS 30000
// A single Li-ion cell reads inside this window (dead about 3,000, full 4,200); anything else is a wrong register or an ADC that is off.
#define BATTERY_MV_MIN 2500
#define BATTERY_MV_MAX 4600

// Store a sample: the fuel gauge's percent and the charge state. A percent outside 0..100 and a state outside 0..2 are
// stored as unknown (-1).
void battery_set(int pct, int chg);
// The percent to SHOW, 0..100, or -1 (no PMIC, not sampled yet, bad read). While the dial runs on its battery (charge state
// 2) this is the gauge's percent mapped to the share of the run time left (battery_discharge_pct); on external power it is
// the gauge's own percent.
int battery_pct(void);
// The gauge's own percent, 0..100, or -1. What the battery log stores.
int battery_pct_raw(void);
// 0 standby, 1 charging, 2 discharging, or -1.
int battery_chg(void);

// The gauge's percent while discharging, mapped to the share of the run time left at a constant current. The AXP2101 gauge
// reads about 58 % with a third of the run left (overnight log, 2026-10-28), because its curve follows the cell's voltage and
// the cell is flat in the middle. -1 for a value outside 0..100. Pure function of its argument.
int battery_discharge_pct(int raw);

// Battery voltage in mV, or -1 (not read, ADC off, outside 2,500..4,600 mV). Set by the one poller, like the percent.
void battery_set_mv(int mv);
int battery_mv(void);
// The two voltage register bytes (AXP2101 VBAT_H bits 5:0 and VBAT_L, 1 mV a step) as mV; -1 for a failed read (negative
// byte) or a value outside 2,500..4,600 mV, which includes 0 (ADC off).
int battery_mv_from_regs(int high, int low);

// One poller's schedule (the caller owns it, zero-initialised).
typedef struct { uint32_t next_ms; bool armed; } battery_poll_t;
// True when a poll is due at `now_ms` (the first call always is), and moves the schedule to now + BATTERY_POLL_MS. Wrap-safe;
// a late call is one poll, not a burst.
bool battery_poll_due(battery_poll_t *poll, uint32_t now_ms);
