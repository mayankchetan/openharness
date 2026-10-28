// battery.c: the cached battery figures the alive line and the hello frame read, and the 30 s poll schedule.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "battery.h"

int main(void)
{
    // Unknown until the first sample.
    assert(battery_pct() == -1 && battery_chg() == -1);
    battery_set(73, 2);
    assert(battery_pct() == 73 && battery_chg() == 2);
    battery_set(100, 0);
    assert(battery_pct() == 100 && battery_chg() == 0);
    // A failed read (-1) or an impossible value is unknown, never a wrong number.
    battery_set(-1, 1);
    assert(battery_pct() == -1 && battery_chg() == 1);
    battery_set(101, 3);
    assert(battery_pct() == -1 && battery_chg() == -1);
    battery_set(0, 2);
    assert(battery_pct() == 0 && battery_chg() == 2);

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

    puts("battery: unknown until sampled, bad values read as unknown, 30 s wrap-safe schedule PASS");
    return 0;
}
