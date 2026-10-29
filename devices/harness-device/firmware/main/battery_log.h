#pragma once
// DEVICE_BATTERY_LOG (default OFF): a battery history in NVS, for working out how long a charge lasts.
// While the dial runs on its battery (charge state 2, battery.c) one sample a minute, {boot, uptime_s, gauge percent, mV},
// goes into a ring of 1,440 samples (battery_ring.c; NVS namespace "batlog", keys b00..b23 and "meta"). When the dial is
// back on external power with the cable session up, the samples not yet shown are logged once, as lines the daemon files in
// the dial log:
//   batlog: BATLOG dump begin
//   batlog: BATLOG seq=<n> boot=<b> up_s=<s> pct=<gauge> mv=<mV> shown=<p>   (oldest first; seq counts samples since the first)
//   batlog: BATLOG dump end n=<count>
// pct is the fuel gauge's own percent, mv the cell voltage (0 = not read) and shown what the dial showed for that gauge
// value (battery_discharge_pct, battery.c), so a later run can refit that table from pct and mv.
//
// A DUMP IS A ONE-TIME READ, BY DESIGN. It marks what it printed as dumped (the "dumped" counter in the meta record); the
// samples themselves stay in the ring until newer ones overwrite them, but a later dump prints only samples recorded after the
// last one. So the second boot after a drain run, plugged in, dumps n=0. The lines are in the dial log of the first dump.
void battery_log_start(void);
