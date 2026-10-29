// The battery rim arc (ui/habitat/battery_rim.c): a 4 px track and a fill at the bottom of the round glass, 60 degrees centred on
// 6 o'clock, on the black ground, clear of everything else the screens draw.
#include "../main/ui/habitat/battery_rim.h"
#include "../main/battery.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t before[HT_WIDTH * HT_HEIGHT], after[HT_WIDTH * HT_HEIGHT];
static const ht_rect_t all = {0, 0, HT_WIDTH, HT_HEIGHT};
enum { NORMAL = 0xADB5, AMBER = 0xFD80, RED = 0xC986, TRACK = 0x2104 };   // RGB565 as the scene stores them
static uint16_t swapped(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }                // the panel byte order ht_raster writes

static ht_scene_t scene(int pct)
{
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    ht_battery_rim(&s, pct, NORMAL, AMBER, RED, TRACK);
    return s;
}
// The pixel at `deg` degrees counted anticlockwise from 3 o'clock, `r` px from the centre of the 466 px glass.
static uint16_t at(const uint16_t *px, double deg, double r)
{
    const double a = deg * 3.14159265358979 / 180.0;
    const int x = (int)floor(233.0 + r * cos(a)), y = (int)floor(233.0 - r * sin(a));
    return px[y * HT_WIDTH + x];
}
static bool covered(const ht_damage_t *d, int x, int y)
{
    for (int i = 0; i < d->count; i++)
        if (x >= d->rect[i].x && x < d->rect[i].x + d->rect[i].w && y >= d->rect[i].y && y < d->rect[i].y + d->rect[i].h) return true;
    return false;
}

int main(void)
{
    // Two runs (track, fill) appended; the scene's other runs and their order are untouched.
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    ht_text(&s, 10, 10, 100, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_battery_rim(&s, 80, NORMAL, AMBER, RED, TRACK) && s.count == 3 && !s.runs[0].ring.set && s.runs[1].ring.set && s.runs[2].ring.set);

    // No room for both: nothing is added (the rim never displaces a screen's own content).
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 1) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(!ht_battery_rim(&s, 80, NORMAL, AMBER, RED, TRACK) && s.count == HT_RUNS - 1);
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 2) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_battery_rim(&s, 80, NORMAL, AMBER, RED, TRACK) && s.count == HT_RUNS);
    // Hidden (-1) adds nothing at all.
    ht_scene_clear(&s, 0);
    assert(ht_battery_rim(&s, -1, NORMAL, AMBER, RED, TRACK) && s.count == 0);

    // The bounds are the strip itself, not a box reaching back to the circle's centre (ht_ring_arc once started them there: 54,522 px,
    // which the raster then walked pixel by pixel with a square root each; the strip is 9,126 px).
    ht_scene_t strip = scene(100);
    ht_rect_t box = ht_run_bounds(&strip.runs[0]);
    assert(box.y >= 420 && box.y + box.h <= HT_HEIGHT && box.x >= 100 && box.x + box.w <= 366 && box.w * box.h < 10000);

    // Full battery: the whole 60 degrees are fill colour, the band is 4 px (radius 226 to 230), nothing outside it or past the ends.
    ht_scene_t full = scene(100);
    ht_raster(&full, all, before);
    for (double deg = 242; deg <= 298; deg += 4) {
        assert(at(before, deg, 228) == swapped(NORMAL));
        assert(at(before, deg, 224) == 0 && at(before, deg, 232) == 0);
    }
    assert(at(before, 200, 228) == 0 && at(before, 330, 228) == 0 && at(before, 270, 150) == 0);
    assert(at(before, 270, 227) != 0 && at(before, 270, 229) != 0);                     // 4 px: 226..230

    // Half battery: the fill is 30 degrees centred on 6 o'clock, the track shows the rest of the 60.
    ht_scene_t half = scene(50);
    ht_raster(&half, all, before);
    assert(at(before, 270, 228) == swapped(NORMAL) && at(before, 258, 228) == swapped(NORMAL) && at(before, 282, 228) == swapped(NORMAL));
    assert(at(before, 248, 228) == swapped(TRACK) && at(before, 292, 228) == swapped(TRACK));
    assert(at(before, 235, 228) == 0 && at(before, 305, 228) == 0);

    // Colours: normal above 20, amber at 20 and below, red at 10 and below.
    ht_scene_t t;
    t = scene(21); ht_raster(&t, all, before); assert(at(before, 270, 228) == swapped(NORMAL));
    t = scene(20); ht_raster(&t, all, before); assert(at(before, 270, 228) == swapped(AMBER));
    t = scene(11); ht_raster(&t, all, before); assert(at(before, 270, 228) == swapped(AMBER));
    t = scene(10); ht_raster(&t, all, before); assert(at(before, 270, 228) == swapped(RED));
    // Empty battery: the track alone (an empty fill slot keeps the run order).
    t = scene(0); ht_raster(&t, all, before);
    assert(t.count == 2 && at(before, 270, 228) == swapped(TRACK) && at(before, 250, 228) == swapped(TRACK));

    // Redraw only what changes. The same percent twice is no damage; a changed percent damages the rim strip only, and a partial
    // paint equals the full raster everywhere.
    unsigned worst = 0;
    for (int a = 0; a <= 100; a += 1) for (int b = 0; b <= 100; b += 7) {
        ht_scene_t one = scene(a), two = scene(b);
        ht_damage_t d;
        ht_damage(&one, &two, &d);
        ht_raster(&one, all, before);
        ht_raster(&two, all, after);
        if (battery_rim_half_deg(a) == battery_rim_half_deg(b) && battery_rim_tier(a) == battery_rim_tier(b)) assert(d.count == 0);
        for (int y = 0; y < HT_HEIGHT; y++) for (int x = 0; x < HT_WIDTH; x++)
            if (before[y * HT_WIDTH + x] != after[y * HT_WIDTH + x]) assert(covered(&d, x, y));
        for (int i = 0; i < d.count; i++) {   // the rim strip only
            const ht_rect_t r = d.rect[i];
            if (!(r.y >= 420 && r.x >= 100 && r.x + r.w <= 366 && r.y + r.h <= HT_HEIGHT)) fprintf(stderr, "damage %d -> %d: rect %d,%d %dx%d\n", a, b, r.x, r.y, r.w, r.h);
            assert(r.y >= 420 && r.x >= 100 && r.x + r.w <= 366 && r.y + r.h <= HT_HEIGHT);
        }
        if (d.pixels > worst) worst = d.pixels;
    }
    assert(worst < 10000);   // under 5 percent of the glass
    printf("battery rim: 4 px band at r 226..230, 60 degrees on 6 o'clock, fill 0..30 degrees a side, amber <= 20, red <= 10, hidden on external power, "
           "needs 2 free runs, partial paints equal full raster, worst damage %u px PASS\n", worst);
    return 0;
}
