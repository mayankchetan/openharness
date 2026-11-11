#include "link_icon.h"

// The Wi-Fi glyph: the dot at the foot, three arcs over it (r 5, 9 and 13, 2 px wide, 80 degrees each, centred straight up).
enum {
    WIFI_DOT_X = 377, WIFI_DOT_Y = 383, WIFI_DOT_D = 4,   // the dot's box; its centre is (379, 385)
    WIFI_CX16 = 379 * 16, WIFI_CY16 = 385 * 16,
    WIFI_BAND16 = 2 * 16, WIFI_MID_DEG = 90, WIFI_HALF_DEG = 40,
};
static const int WIFI_RADIUS16[3] = {5 * 16, 9 * 16, 13 * 16};

bool ht_link_icon(ht_scene_t *s, ht_link_kind_t kind, bool up, const ht_link_icon_ink_t *ink)
{
    if (kind != HT_LINK_WIFI && kind != HT_LINK_PHONE) return true;
    if (s->count + HT_LINK_ICON_RUNS > HT_RUNS) return false;
    const uint16_t c = up ? ink->normal : ink->warn;
    if (kind == HT_LINK_WIFI) {
        for (int i = 0; i < 3; i++)
            ht_ring_arc(s, WIFI_CX16, WIFI_CY16, WIFI_RADIUS16[i], WIFI_BAND16, WIFI_MID_DEG, WIFI_HALF_DEG, c);
        ht_box(s, WIFI_DOT_X, WIFI_DOT_Y, WIFI_DOT_D, WIFI_DOT_D, WIFI_DOT_D / 2, c, c);
        return true;
    }
    // The phone: a 12 x 20 outline (1 px border, the ground inside), a speaker slot near the top and a home dot near the foot.
    ht_box(s, 373, 369, 12, 20, 3, ink->ground, c);
    ht_box(s, 377, 372, 4, 2, 1, c, c);
    ht_box(s, 377, 384, 4, 3, 1, c, c);
    return true;
}
