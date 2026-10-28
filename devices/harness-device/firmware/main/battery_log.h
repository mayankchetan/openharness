#pragma once
// DEVICE_BATTERY_LOG (default OFF): a battery history in NVS, for working out how long a charge lasts.
// While the dial runs on its battery (charge state 2, battery.c) one sample a minute, {boot, uptime_s, percent}, goes into
// a ring of 1,440 samples (battery_ring.c; NVS namespace "batlog", keys b00..b23 and "meta"). When the dial is back on
// external power with the cable session up, the samples not yet shown are logged once, as lines the daemon files in the
// dial log:
//   batlog: BATLOG dump begin
//   batlog: BATLOG seq=<n> boot=<b> up_s=<s> pct=<p>      (oldest first; seq counts samples since the first)
//   batlog: BATLOG dump end n=<count>
// No voltage: the AXP2101 voltage registers are not in the repo's parts files or in a datasheet it holds, so none is read.
void battery_log_start(void);
