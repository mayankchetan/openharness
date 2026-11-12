#include "brightness_arc.h"
#include <math.h>

enum { CENTRE = 233 };

bool ht_bright_arc_hit(int x, int y)
{
    const int dx = x - CENTRE, dy = y - CENTRE;
    if (dx <= 0) return false;   // the arc is on the right half only
    const int r2 = dx * dx + dy * dy;
    if (r2 < 170 * 170 || r2 > 240 * 240) return false;   // the band, 205..215, and a thumb's width on both sides of it
    return dy * dy * 100 <= 85 * r2;   // within 67 degrees of 3 o'clock: the arc's 60 and a little slack at both ends
}

int ht_bright_percent_at(int y)
{
    if (y <= HT_BRIGHT_Y_TOP) return HT_BRIGHT_MAX;
    if (y >= HT_BRIGHT_Y_BOT) return HT_BRIGHT_MIN;
    const int span = HT_BRIGHT_Y_BOT - HT_BRIGHT_Y_TOP, steps = HT_BRIGHT_MAX - HT_BRIGHT_MIN;
    return HT_BRIGHT_MIN + ((HT_BRIGHT_Y_BOT - y) * steps + span / 2) / span;
}

void ht_bright_knob(int percent, int *x, int *y)
{
    if (percent < HT_BRIGHT_MIN) percent = HT_BRIGHT_MIN;
    if (percent > HT_BRIGHT_MAX) percent = HT_BRIGHT_MAX;
    const int span = HT_BRIGHT_Y_BOT - HT_BRIGHT_Y_TOP, steps = HT_BRIGHT_MAX - HT_BRIGHT_MIN;
    const int ky = HT_BRIGHT_Y_BOT - ((percent - HT_BRIGHT_MIN) * span + steps / 2) / steps;
    const float up = (float)(CENTRE - ky);
    *y = ky;
    *x = CENTRE + (int)lroundf(sqrtf((float)(HT_BRIGHT_R * HT_BRIGHT_R) - up * up));
}

bool ht_bright_arc(ht_scene_t *s, int percent, const ht_bright_ink_t *ink)
{
    if (s->count + HT_BRIGHT_RUNS > HT_RUNS) return false;
    int kx, ky;
    ht_bright_knob(percent, &kx, &ky);
    // The fill's far end, in whole degrees: where the knob's height meets the circle. The arc is drawn as a middle and a half width, so the two ends
    // must add to an even number of degrees; an odd one is taken one degree further (under 4 px, and the knob covers it).
    int top = (int)lroundf(asinf((float)(CENTRE - ky) / (float)HT_BRIGHT_R) * 57.29578f);
    if (top > HT_BRIGHT_HALF_DEG) top = HT_BRIGHT_HALF_DEG;
    if (top < -HT_BRIGHT_HALF_DEG) top = -HT_BRIGHT_HALF_DEG;
    if ((top & 1) && top < HT_BRIGHT_HALF_DEG) top++;
    const int half = (top + HT_BRIGHT_HALF_DEG) / 2, mid = top - half;
    const int c16 = CENTRE * 16;
    // The track first, so the fill is drawn over it, then the knob over both.
    ht_ring_arc(s, c16, c16, HT_BRIGHT_R * 16, HT_BRIGHT_BAND * 16, 0, HT_BRIGHT_HALF_DEG, ink->track);
    ht_ring_arc(s, c16, c16, HT_BRIGHT_R * 16, half > 0 ? HT_BRIGHT_BAND * 16 : 0, mid, half, ink->fill);
    ht_box(s, kx - HT_BRIGHT_KNOB / 2, ky - HT_BRIGHT_KNOB / 2, HT_BRIGHT_KNOB, HT_BRIGHT_KNOB, HT_BRIGHT_KNOB / 2, ink->knob, ink->knob);
    return true;
}
