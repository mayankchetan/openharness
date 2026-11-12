// The Brightness arc: the slider of the Brightness screen (ui_habitat.c). A dim track, a green fill and a knob on the right edge of the round glass,
// from 5 o'clock up to 1 o'clock. Up is brighter. Pure geometry and scene building (three runs, no allocation), so a host test runs it against the
// real compositor.
//
// WHERE. The band is r 205..215 about the centre (233, 233), mid line r 210, over 60 degrees either side of 3 o'clock. It is 11 px inside the battery
// rim's band (r 226..230) and ends at the rim arc's own end at 5 o'clock without touching it. The link icon (4:30, r about 207) sits in the same place;
// the Brightness screen does not draw it.
//
// THE FINGER. A drag is mapped by its height, not its angle, so a straight pull up the glass reaches 100 and the knob still sits on the arc: the
// knob's height is the finger's, its x is the arc's at that height. The range is 8 to 100 percent; 8 is the panel's floor (display_set_brightness).
#pragma once
#include "terminal.h"

enum {
    HT_BRIGHT_MIN = 8,                // the floor of the range: display_set_brightness clamps lower values to this
    HT_BRIGHT_MAX = 100,
    HT_BRIGHT_R = 210,                // the middle of the band, px from the glass centre
    HT_BRIGHT_BAND = 10,              // the band's width
    HT_BRIGHT_HALF_DEG = 60,          // the arc spans 3 o'clock +- this: 5 o'clock up to 1 o'clock
    HT_BRIGHT_Y_TOP = 51,             // the arc's top and bottom heights: 233 -+ 182 (210 * sin 60 degrees = 181.9)
    HT_BRIGHT_Y_BOT = 415,
    HT_BRIGHT_KNOB = 24,              // the knob is a 24 px disc
    HT_BRIGHT_RUNS = 3,               // track, fill, knob. Fewer free runs than this and nothing is added.
};

// The colours (RGB565 as the scene stores them, already mixed over the ground): the track, the fill and the knob.
typedef struct { uint16_t track, fill, knob; } ht_bright_ink_t;

// True when a touch at (x, y) starts a drag: on the right half of the glass, 170 to 240 px from the centre (the arc and a thumb's width on both sides),
// between 5 and 1 o'clock with 7 degrees of slack at both ends. Nothing else on this screen is in that band.
bool ht_bright_arc_hit(int x, int y);
// The percent for a finger at height `y`: HT_BRIGHT_MAX at the top of the arc, HT_BRIGHT_MIN at the bottom, a straight line between; a `y` beyond either
// end gives that end.
int ht_bright_percent_at(int y);
// The knob's centre for `percent` (clamped to the range): on the band's mid line.
void ht_bright_knob(int percent, int *x, int *y);
// Appends the track, the fill (from 5 o'clock up to the knob) and the knob, in that order. With fewer than HT_BRIGHT_RUNS free runs nothing is added and
// the result is false. True otherwise. At the floor the fill is an empty placeholder: the run order never changes.
bool ht_bright_arc(ht_scene_t *scene, int percent, const ht_bright_ink_t *ink);
