#include "battery.h"

static volatile int s_pct = -1, s_chg = -1;

void battery_set(int pct, int chg)
{
    s_pct = pct >= 0 && pct <= 100 ? pct : -1;
    s_chg = chg >= 0 && chg <= 2 ? chg : -1;
}
int battery_pct(void) { return s_pct; }
int battery_chg(void) { return s_chg; }

bool battery_poll_due(battery_poll_t *poll, uint32_t now_ms)
{
    if (poll->armed && (int32_t)(now_ms - poll->next_ms) < 0) return false;
    poll->armed = true;
    poll->next_ms = now_ms + BATTERY_POLL_MS;
    return true;
}
