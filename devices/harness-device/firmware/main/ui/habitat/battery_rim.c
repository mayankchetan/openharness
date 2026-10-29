#include "battery_rim.h"
#include "../../battery.h"
#include <stdio.h>
#include <string.h>

bool ht_battery_rim(ht_scene_t *s, int pct, bool number, const ht_battery_rim_ink_t *ink)
{
    if (pct < 0) return true;
    if (s->count + 2 > HT_RUNS) return false;
    const int tier = battery_rim_tier(pct), half = battery_rim_half_deg(pct);
    // The track first, so the fill is drawn over it.
    ht_ring_arc(s, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_RADIUS, HT_BATTERY_RIM_WIDTH,
                HT_BATTERY_RIM_MID_DEG, BATTERY_RIM_HALF_DEG, ink->track);
    ht_ring_arc(s, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_CENTRE, HT_BATTERY_RIM_RADIUS, half ? HT_BATTERY_RIM_WIDTH : 0,
                HT_BATTERY_RIM_MID_DEG, half, tier == 2 ? ink->red : tier == 1 ? ink->amber : ink->normal);
    if (number && s->count < HT_RUNS) {
        // Centred on the glass, its bottom 3 px above the band's inner edge (226 px below the centre).
        char text[8];
        snprintf(text, sizeof text, "%d%%", pct > 100 ? 100 : pct);
        const ht_font_t *font = HT_BATTERY_RIM_FONT;
        const int w = (int)strlen(text) * font->width;
        ht_text(s, 233 - w / 2, 233 + 226 - 3 - font->height, w, font, tier == 2 ? ink->red : tier == 1 ? ink->amber : ink->text,
                ink->text_bg, text);
    }
    return true;
}
