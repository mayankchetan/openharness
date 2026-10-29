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
enum { NORMAL = 0xADB5, AMBER = 0xFD80, RED = 0xC986, TRACK = 0x2104, TEXT = 0xEF3B, TEXT_BG = 0x0841 };   // RGB565 as the scene stores them
static const ht_battery_rim_ink_t ink = {NORMAL, AMBER, RED, TRACK, TEXT, TEXT_BG};
static uint16_t swapped(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }                // the panel byte order ht_raster writes

static ht_scene_t scene_n(int pct, bool number)
{
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    ht_battery_rim(&s, pct, number, &ink);
    return s;
}
static ht_scene_t scene(int pct) { return scene_n(pct, false); }
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
    assert(ht_battery_rim(&s, 80, false, &ink) && s.count == 3 && !s.runs[0].ring.set && s.runs[1].ring.set && s.runs[2].ring.set);

    // No room for both: nothing is added (the rim never displaces a screen's own content).
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 1) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(!ht_battery_rim(&s, 80, false, &ink) && s.count == HT_RUNS - 1);
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 2) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_battery_rim(&s, 80, false, &ink) && s.count == HT_RUNS);
    // Hidden (-1) adds nothing at all.
    ht_scene_clear(&s, 0);
    assert(ht_battery_rim(&s, -1, false, &ink) && s.count == 0);

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

    // THE NUMBER (3 s after a wake, asked for by `number`): one more text run, last, "73%" centred just inside the arc at 6 o'clock, in
    // the fill's tier colour (the normal tier in the text ink), on a near-black backing so it reads over a screen's own content.
    {
        ht_scene_t n = scene_n(73, true);
        assert(n.count == 3 && !n.runs[2].ring.set && n.runs[2].font == &ht_mono_20 && !strcmp(n.runs[2].text, "73%"));
        assert(n.runs[2].fg == TEXT && n.runs[2].bg == TEXT_BG);
        const ht_rect_t tb = ht_run_bounds(&n.runs[2]);
        assert(tb.w == 3 * ht_mono_20.width && tb.h == ht_mono_20.height);
        assert(tb.x + tb.w / 2 >= 232 && tb.x + tb.w / 2 <= 234);                       // centred on the glass
        assert(tb.y + tb.h <= 459 - 2);                                                 // clear of the band (inner edge y 459 at 6 o'clock)
        assert(tb.y >= 420);                                                            // inside the rim strip the damage test knows
        assert(!strcmp(scene_n(100, true).runs[2].text, "100%") && !strcmp(scene_n(5, true).runs[2].text, "5%") &&
               !strcmp(scene_n(0, true).runs[2].text, "0%"));
        assert(scene_n(20, true).runs[2].fg == AMBER && scene_n(11, true).runs[2].fg == AMBER && scene_n(10, true).runs[2].fg == RED &&
               scene_n(0, true).runs[2].fg == RED && scene_n(21, true).runs[2].fg == TEXT);
        // The arc runs are the same with or without the number, and the number touches no arc pixel.
        ht_scene_t plain = scene(73);
        assert(!memcmp(&n.runs[0], &plain.runs[0], sizeof n.runs[0]) && !memcmp(&n.runs[1], &plain.runs[1], sizeof n.runs[1]));
        ht_raster(&plain, all, before);
        ht_raster(&n, all, after);
        for (int y = 0; y < HT_HEIGHT; y++) for (int x = 0; x < HT_WIDTH; x++) {
            const bool in_box = x >= tb.x && x < tb.x + tb.w && y >= tb.y && y < tb.y + tb.h;
            if (before[y * HT_WIDTH + x] != after[y * HT_WIDTH + x]) assert(in_box);   // nothing outside the text box changes
            if (in_box) assert(hypot(x + 0.5 - 233.0, y + 0.5 - 233.0) < 224.0);        // and the box is inside the band's inner edge
        }
        int lit = 0;
        for (int y = tb.y; y < tb.y + tb.h; y++) for (int x = tb.x; x < tb.x + tb.w; x++) lit += after[y * HT_WIDTH + x] == swapped(TEXT);
        assert(lit > 20);                                                               // the glyphs are really drawn
        // Not asked for: no text run. Hidden (-1): nothing, number or not.
        assert(scene_n(73, false).count == 2 && scene_n(-1, true).count == 0);
        // The arc has priority: with two free runs the arc is drawn and the number is skipped; with three both.
        ht_scene_clear(&s, 0);
        while (s.count < HT_RUNS - 2) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
        assert(ht_battery_rim(&s, 80, true, &ink) && s.count == HT_RUNS && s.runs[HT_RUNS - 1].ring.set);
        ht_scene_clear(&s, 0);
        while (s.count < HT_RUNS - 3) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
        assert(ht_battery_rim(&s, 80, true, &ink) && s.count == HT_RUNS && !s.runs[HT_RUNS - 1].ring.set && !strcmp(s.runs[HT_RUNS - 1].text, "80%"));
    }
    // The number comes, changes and goes with exact damage: its box only, and every partial paint equals the full raster.
    {
        unsigned worst_number = 0;
        for (int a = 0; a <= 100; a += 3) for (int b = 0; b <= 100; b += 5) for (int mode = 0; mode < 4; mode++) {
            const bool na = mode & 1, nb = mode >> 1;
            ht_scene_t one = scene_n(a, na), two = scene_n(b, nb);
            ht_damage_t d;
            ht_damage(&one, &two, &d);
            ht_raster(&one, all, before);
            ht_raster(&two, all, after);
            for (int y = 0; y < HT_HEIGHT; y++) for (int x = 0; x < HT_WIDTH; x++)
                if (before[y * HT_WIDTH + x] != after[y * HT_WIDTH + x]) assert(covered(&d, x, y));
            for (int i = 0; i < d.count; i++) assert(d.rect[i].y >= 420 && d.rect[i].x >= 100 && d.rect[i].x + d.rect[i].w <= 366 && d.rect[i].y + d.rect[i].h <= HT_HEIGHT);
            if (d.pixels > worst_number) worst_number = d.pixels;
            // Same percent, number only toggled: the text box and nothing else.
            if (a == b && na != nb) assert(d.count >= 1 && d.pixels <= 50 * 28);
            // Same percent, same number state: no damage at all.
            if (a == b && na == nb) assert(d.count == 0);
        }
        assert(worst_number < 12000);
        printf("battery rim number: centred 'N%%' in ht_mono_20 inside the band, tier colours, arc has priority over it, exact damage, worst %u px\n", worst_number);
    }

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
