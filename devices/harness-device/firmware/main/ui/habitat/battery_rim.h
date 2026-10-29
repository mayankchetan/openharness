// The battery rim: a thin arc at the bottom edge of the round glass while the dial runs on its battery (ui_habitat.c adds it to
// every screen's scene, last). A 4 px band at the rim, 60 degrees wide and centred on 6 o'clock; a dim track shows the whole span
// and the fill, centred in it, shrinks with the percent. Pure scene building (two ring-arc runs, no allocation), so a host test
// runs it against the real compositor.
//
// WHY THERE. The glass is 466 px across and the curved text sits inside radius 230 (ink at most about 222 on the lower arc,
// whose descenders point at the rim); the band is r 226..230 about the centre (233, 233), 3 px inside the edge. No screen
// draws a pixel in it: the host render of all 112 gallery items was checked for black pixels there, and the voice sound waves
// (the only other ring arcs) sit around the pet, far inside. The retired rim-scroll gesture drew nothing.
#pragma once
#include "terminal.h"

enum {
    HT_BATTERY_RIM_CENTRE = 233 * 16,   // the glass centre, sixteenths of a pixel (ht_ring_arc units)
    HT_BATTERY_RIM_RADIUS = 228 * 16,   // the middle of the band
    HT_BATTERY_RIM_WIDTH = 4 * 16,
    HT_BATTERY_RIM_MID_DEG = 270,       // 6 o'clock; degrees run anticlockwise from 3 o'clock
};

// Adds the track and the fill (two runs) for `pct`, 0..100; `pct` below 0 adds nothing. The fill takes the colour of its tier
// (battery_rim_tier: normal, amber at 20 and below, red at 10 and below); an empty fill keeps its slot as a placeholder, so the
// run order does not change as the percent falls. If the scene has fewer than two free runs nothing is added and the result is
// false: the rim never displaces a screen's own content. True otherwise.
bool ht_battery_rim(ht_scene_t *scene, int pct, uint16_t normal, uint16_t amber, uint16_t red, uint16_t track);
