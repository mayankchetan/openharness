#include "battery_rim.h"
#include "../../battery.h"

bool ht_battery_rim(ht_scene_t *s, int pct, uint16_t normal, uint16_t amber, uint16_t red, uint16_t track)
{
    if (pct < 0) return true;
    if (s->count + 2 > HT_RUNS) return false;
    const int tier = battery_rim_tier(pct), half = battery_rim_half_deg(pct);
    // The track first, so the fill is drawn over it.
    ht_ring_arc(s, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_RADIUS, HT_BATTERY_RIM_WIDTH,
                HT_BATTERY_RIM_MID_DEG, BATTERY_RIM_HALF_DEG, track);
    ht_ring_arc(s, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_RADIUS, half ? HT_BATTERY_RIM_WIDTH : 0,
                HT_BATTERY_RIM_MID_DEG, half, tier == 2 ? red : tier == 1 ? amber : normal);
    return true;
}
