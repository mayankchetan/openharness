// battery.c: the cached battery figures the alive line and the hello frame read, and the 30 s poll schedule.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "battery.h"

int main(void)
{
    // Unknown until the first sample.
    assert(battery_pct() == -1 && battery_pct_raw() == -1 && battery_chg() == -1 && battery_mv() == -1);
    // On external power (standby 0, charging 1) the gauge's percent is shown as it is.
    battery_set(73, 0);
    assert(battery_pct() == 73 && battery_pct_raw() == 73 && battery_chg() == 0);
    battery_set(73, 1);
    assert(battery_pct() == 73 && battery_pct_raw() == 73 && battery_chg() == 1);
    battery_set(100, 0);
    assert(battery_pct() == 100 && battery_chg() == 0);
    // On the battery (2) the shown percent follows the remaining time (battery_discharge_pct); the raw one stays available.
    battery_set(73, 2);
    assert(battery_pct_raw() == 73 && battery_chg() == 2 && battery_pct() == battery_discharge_pct(73) && battery_pct() < 73);
    // A failed read (-1) or an impossible value is unknown, never a wrong number.
    battery_set(-1, 1);
    assert(battery_pct() == -1 && battery_chg() == 1);
    battery_set(101, 3);
    assert(battery_pct() == -1 && battery_chg() == -1);
    battery_set(0, 2);
    assert(battery_pct() == 0 && battery_chg() == 2);

    // The gauge's discharge curve against time: the overnight log of 2026-10-28 (one boot, one sample a minute, dial idle,
    // dead at 196 min). With a constant current the remaining share is 1 - t / 196. {minutes, percent the gauge showed}.
    static const int drain[][2] = {{0,100},{9,100},{19,95},{29,91},{39,88},{49,85},{59,83},{69,79},{79,75},{89,72},{99,68},
        {109,65},{119,62},{129,58},{139,53},{149,44},{159,33},{169,20},{179,11},{189,2},{196,0}};
    int worst = 0;
    for (unsigned i = 0; i < sizeof drain / sizeof drain[0]; i++) {
        int truth = (int)(100.0 * (196 - drain[i][0]) / 196 + 0.5), shown = battery_discharge_pct(drain[i][1]);
        int off = shown > truth ? shown - truth : truth - shown;
        if (drain[i][0] == 9) continue;   // the gauge still said 100 here: one reading cannot be both 100 and 95
        if (off > worst) worst = off;
    }
    assert(worst <= 1);                                          // every logged point within 1 percentage point of the time left
    assert(battery_discharge_pct(58) >= 33 && battery_discharge_pct(58) <= 35);   // 58 % at 129 min is 34 % of the charge left
    assert(battery_discharge_pct(0) == 0 && battery_discharge_pct(100) == 100);
    for (int raw = 1; raw <= 100; raw++) assert(battery_discharge_pct(raw) >= battery_discharge_pct(raw - 1));   // never goes backwards
    assert(battery_discharge_pct(-1) == -1 && battery_discharge_pct(101) == -1);
    battery_set(58, 2);
    assert(battery_pct() >= 33 && battery_pct() <= 35 && battery_pct_raw() == 58);

    // Millivolts: unknown (-1) until set, kept as given inside the plausible window, otherwise unknown. Two register bytes
    // (battery_mv_from_regs: 6 high bits, 8 low bits) are one value in mV, and a failed read or an odd value is -1.
    battery_set_mv(3812);
    assert(battery_mv() == 3812);
    battery_set_mv(0); assert(battery_mv() == -1);
    battery_set_mv(9999); assert(battery_mv() == -1);
    battery_set_mv(4200); assert(battery_mv() == 4200);
    assert(battery_mv_from_regs(0x0F, 0xA4) == 4004 && battery_mv_from_regs(0xCF, 0xA4) == 4004);   // bits 7:6 of the high byte are not data
    assert(battery_mv_from_regs(-1, 0x10) == -1 && battery_mv_from_regs(0x0F, -1) == -1);
    assert(battery_mv_from_regs(0, 0) == -1 && battery_mv_from_regs(0x3F, 0xFF) == -1);      // 0 mV (ADC off) and 16,383 mV

    // The poll schedule: the first call is due at once; then every BATTERY_POLL_MS, wrap-safe, no burst after a stall.
    battery_poll_t poll = {0};
    assert(battery_poll_due(&poll, 1000) && poll.next_ms == 1000 + BATTERY_POLL_MS);
    assert(!battery_poll_due(&poll, 1000 + BATTERY_POLL_MS - 1));
    assert(battery_poll_due(&poll, 1000 + BATTERY_POLL_MS) && poll.next_ms == 1000 + 2 * BATTERY_POLL_MS);
    assert(battery_poll_due(&poll, 1000 + 10 * BATTERY_POLL_MS) && poll.next_ms == 1000 + 11 * BATTERY_POLL_MS);   // late: one poll
    assert(!battery_poll_due(&poll, 1000 + 10 * BATTERY_POLL_MS + 1));
    uint32_t wrap = UINT32_MAX - 5000;
    battery_poll_t other = {0};
    assert(battery_poll_due(&other, wrap) && !battery_poll_due(&other, wrap + 1000));
    assert(battery_poll_due(&other, wrap + BATTERY_POLL_MS));   // crosses the 32-bit wrap
    battery_poll_t zero = {0};
    assert(battery_poll_due(&zero, 0) && !battery_poll_due(&zero, 1));   // time 0 is a time like any other
    assert(BATTERY_POLL_MS == 30000);

    puts("battery: unknown until sampled, bad values read as unknown, discharge curve tracks the time left, mV window, 30 s wrap-safe schedule PASS");
    return 0;
}
