// The permanent link icon (wifi-first work, spec 2026-11-11 section 3.6): a small mark at 4:30 on the round glass, shown on every screen while a wireless mode
// runs, last in the scene like the battery rim (ui_habitat.c adds it after render_battery_rim). A Wi-Fi glyph (three ring arcs and a dot) while the Wi-Fi link
// runs, a phone glyph (a portrait outline, a speaker slot and a home dot) while the phone link runs. Normal ink while the session is up, amber (the theme's
// WARN) while it is not. No text, no tap target, no heap and no bitmap: 3 or 4 scene runs built from ring arcs and rounded boxes, so a host test runs it
// against the real compositor (test_link_icon.c).
//
// WHERE. 20 x 20 px centred at (379, 379): 4:30, r about 207 about the centre (233, 233). Its farthest ink is under 221 px from the centre, 5 px inside the
// battery band (r 226 to 230) and 15 degrees clear of that arc's end (it spans 240 to 300 degrees); the percent number is 140 px away. The only other marks
// near it are the "Switching in a moment" line (Focus face: it ends at x 368, the icon starts at x 369) and the lower-arc status text, which reaches 4:30
// only when it is longer than about 20 characters (the relaxing and Listening lines stop short; test_link_icon.c measures both).
#pragma once
#include "terminal.h"

enum {
    HT_LINK_ICON_CX = 379,     // the centre of the 20 x 20 box
    HT_LINK_ICON_CY = 379,
    HT_LINK_ICON_SIZE = 20,
    HT_LINK_ICON_RUNS = 4,     // the most it uses (Wi-Fi; the phone glyph uses 3). Fewer free runs than this and the icon is skipped.
};
typedef enum { HT_LINK_NONE = 0, HT_LINK_WIFI, HT_LINK_PHONE } ht_link_kind_t;

// The colours (RGB565 as the scene stores them): the ink while the session is up, the warning ink while it is not, and the ground the phone outline is
// filled with (a near black; a rounded box paints its interior).
typedef struct { uint16_t normal, warn, ground; } ht_link_icon_ink_t;

// Appends the glyph for `kind`; `up` picks the ink. HT_LINK_NONE (USB mode), or a number that is no kind, adds nothing and is true. With fewer than
// HT_LINK_ICON_RUNS free runs nothing is added and the result is false: the icon never displaces a screen's own content (the battery rim's rule).
bool ht_link_icon(ht_scene_t *scene, ht_link_kind_t kind, bool up, const ht_link_icon_ink_t *ink);
