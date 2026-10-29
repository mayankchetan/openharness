#include "battery.h"

static volatile int s_pct = -1, s_chg = -1, s_mv = -1;

void battery_set(int pct, int chg)
{
    s_pct = pct >= 0 && pct <= 100 ? pct : -1;
    s_chg = chg >= 0 && chg <= 2 ? chg : -1;
}
int battery_pct_raw(void) { return s_pct; }
int battery_pct(void)
{
    const int raw = s_pct;
    return s_chg == 2 ? battery_discharge_pct(raw) : raw;
}
int battery_chg(void) { return s_chg; }

// The gauge's percent against the share of the run left. Source: the overnight drain log of 2026-10-28 (DEVICE_BATTERY_LOG
// image, one boot, one sample a minute, dial idle, screen on, dead at 196 min), minutes on battery -> percent shown:
//   0:100  9:100 19:95 29:91 39:88 49:85 59:83 69:79 79:75 89:72 99:68 109:65 119:62 129:58 139:53 149:44 159:33 169:20 179:11 189:2 196:0
// With a constant current the share left is 1 - t / 196. Each row below is {shown, 100 * (196 - t) / 196 rounded}; the
// gauge's 100 at 9 min is left out (one reading cannot be both 100 and 95), so 100 stays 100. Between rows: straight lines.
// The old behaviour, before this table, was "shown as the gauge says" (battery_pct() == the AXP2101 register 0xA4 value).
// It is a measurement of one cell at an idle load: a screen-on, voice-heavy hour drains faster, and a new fit needs a new
// run of the log (the log stores the gauge's own percent and the voltage for that).
static const struct { uint8_t raw, left; } k_discharge[] = {
    {0, 0}, {2, 4}, {11, 9}, {20, 14}, {33, 19}, {44, 24}, {53, 29}, {58, 34}, {62, 39}, {65, 44},
    {68, 49}, {72, 55}, {75, 60}, {79, 65}, {83, 70}, {85, 75}, {88, 80}, {91, 85}, {95, 90}, {100, 100},
};
int battery_discharge_pct(int raw)
{
    if (raw < 0 || raw > 100) return -1;
    const unsigned n = sizeof k_discharge / sizeof k_discharge[0];
    for (unsigned i = 1; i < n; i++) {
        if (raw > k_discharge[i].raw) continue;
        const int x0 = k_discharge[i - 1].raw, y0 = k_discharge[i - 1].left;
        const int dx = k_discharge[i].raw - x0, dy = k_discharge[i].left - y0;
        return y0 + (dy * (raw - x0) + dx / 2) / dx;
    }
    return 100;
}

void battery_set_mv(int mv) { s_mv = mv >= BATTERY_MV_MIN && mv <= BATTERY_MV_MAX ? mv : -1; }
int battery_mv(void) { return s_mv; }
int battery_mv_from_regs(int high, int low)
{
    if (high < 0 || low < 0) return -1;
    const int mv = ((high & 0x3F) << 8) | (low & 0xFF);
    return mv >= BATTERY_MV_MIN && mv <= BATTERY_MV_MAX ? mv : -1;
}

bool battery_poll_due(battery_poll_t *poll, uint32_t now_ms)
{
    if (poll->armed && (int32_t)(now_ms - poll->next_ms) < 0) return false;
    poll->armed = true;
    poll->next_ms = now_ms + BATTERY_POLL_MS;
    return true;
}
