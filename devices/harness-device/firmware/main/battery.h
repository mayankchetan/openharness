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

// Store a sample. A percent outside 0..100 and a state outside 0..2 are stored as unknown (-1).
void battery_set(int pct, int chg);
// Percent 0..100, or -1 (no PMIC, not sampled yet, bad read).
int battery_pct(void);
// 0 standby, 1 charging, 2 discharging, or -1.
int battery_chg(void);

// One poller's schedule (the caller owns it, zero-initialised).
typedef struct { uint32_t next_ms; bool armed; } battery_poll_t;
// True when a poll is due at `now_ms` (the first call always is), and moves the schedule to now + BATTERY_POLL_MS. Wrap-safe;
// a late call is one poll, not a burst.
bool battery_poll_due(battery_poll_t *poll, uint32_t now_ms);
