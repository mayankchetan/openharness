// The Brightness arc (ui/habitat/brightness_arc.c): a dim track, a green fill and a knob on the right edge of the round glass, from 5 o'clock up
// to 1 o'clock, with the touch band that starts a drag and the mapping from a finger's height to a percent (8 to 100).
#include "../main/ui/habitat/brightness_arc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t px[HT_WIDTH * HT_HEIGHT];
static const ht_rect_t all = {0, 0, HT_WIDTH, HT_HEIGHT};
enum { TRACK = 0x2104, FILL = 0x05E7, KNOB = 0xEF3B };   // RGB565 as the scene stores them
static const ht_bright_ink_t ink = {TRACK, FILL, KNOB};
static uint16_t swapped(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }   // the panel byte order ht_raster writes

// The pixel `r` px from the glass centre at `deg` degrees, counted anticlockwise from 3 o'clock.
static uint16_t at(double deg, double r)
{
    const double a = deg * 3.14159265358979 / 180.0;
    const int x = (int)floor(233.0 + r * cos(a)), y = (int)floor(233.0 - r * sin(a));
    return px[y * HT_WIDTH + x];
}
static void draw(int percent)
{
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    assert(ht_bright_arc(&s, percent, &ink));
    ht_raster(&s, all, px);
}

int main(void)
{
    // The finger's height to a percent: the top of the arc is 100, the bottom 8, a straight line between, and it never leaves 8..100.
    assert(ht_bright_percent_at(HT_BRIGHT_Y_TOP) == 100 && ht_bright_percent_at(HT_BRIGHT_Y_BOT) == HT_BRIGHT_MIN);
    assert(ht_bright_percent_at(0) == 100 && ht_bright_percent_at(-500) == 100);
    assert(ht_bright_percent_at(465) == HT_BRIGHT_MIN && ht_bright_percent_at(9000) == HT_BRIGHT_MIN);
    int previous = 101;
    for (int y = 0; y < HT_HEIGHT; y++) {
        const int p = ht_bright_percent_at(y);
        assert(p >= HT_BRIGHT_MIN && p <= HT_BRIGHT_MAX && p <= previous);   // up is brighter: lower y, higher percent
        previous = p;
    }
    // Every percent has a knob height that maps back to the same percent: the knob sits where the finger would have to be.
    for (int p = HT_BRIGHT_MIN; p <= HT_BRIGHT_MAX; p++) {
        int x, y;
        ht_bright_knob(p, &x, &y);
        assert(ht_bright_percent_at(y) == p);
        const double r = hypot(x - 233.0, y - 233.0);
        assert(fabs(r - HT_BRIGHT_R) < 1.0 && x > 233);                 // on the band's middle line, right of the centre
        assert(x + HT_BRIGHT_KNOB / 2 <= 233 + 229 && y - HT_BRIGHT_KNOB / 2 >= 0 && y + HT_BRIGHT_KNOB / 2 < HT_HEIGHT);   // the knob stays on the glass
    }
    // A percent outside the range is clamped, not drawn off the arc.
    int x0, y0, x1, y1;
    ht_bright_knob(-5, &x0, &y0); ht_bright_knob(HT_BRIGHT_MIN, &x1, &y1);
    assert(x0 == x1 && y0 == y1);
    ht_bright_knob(500, &x0, &y0); ht_bright_knob(HT_BRIGHT_MAX, &x1, &y1);
    assert(x0 == x1 && y0 == y1);

    // The touch band: on the arc, anywhere from 5 o'clock to 1 o'clock, a thumb's width either side of it; nothing else.
    for (double deg = -60; deg <= 60; deg += 5) {
        const double a = deg * 3.14159265358979 / 180.0;
        for (double r = 180; r <= 232; r += 4)
            assert(ht_bright_arc_hit((int)lround(233 + r * cos(a)), (int)lround(233 - r * sin(a))) && "the arc and its margins start a drag");
    }
    assert(!ht_bright_arc_hit(233, 233) && !ht_bright_arc_hit(300, 233) && !ht_bright_arc_hit(380, 233));   // the number and the middle of the glass
    assert(!ht_bright_arc_hit(110, 85) && !ht_bright_arc_hit(60, 233) && !ht_bright_arc_hit(30, 233));     // the back control and the left edge
    assert(!ht_bright_arc_hit(233, 20) && !ht_bright_arc_hit(233, 445));                                     // 12 and 6 o'clock
    assert(!ht_bright_arc_hit(150, 440) && !ht_bright_arc_hit(150, 30));                                     // the left side of the top and bottom
    assert(!ht_bright_arc_hit(233 + 35, 233 + 200) && !ht_bright_arc_hit(233 + 35, 233 - 200));              // past the arc's ends (80 degrees)
    assert(!ht_bright_arc_hit(-1, 233) && !ht_bright_arc_hit(233 + 300, 233));                                // not even on the glass

    // Three runs, appended last; the scene's other runs are untouched.
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    ht_text(&s, 10, 10, 100, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_bright_arc(&s, 50, &ink) && s.count == 4 && !s.runs[0].ring.set && s.runs[1].ring.set && s.runs[2].ring.set && s.runs[3].box.h);
    // No room for all three: nothing is added (the arc never displaces a screen's own content), and the result says so.
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 2) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(!ht_bright_arc(&s, 50, &ink) && s.count == HT_RUNS - 2);
    ht_scene_clear(&s, 0);
    while (s.count < HT_RUNS - 3) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_bright_arc(&s, 50, &ink) && s.count == HT_RUNS);

    // 100 percent: the fill covers the whole arc, from 5 o'clock to 1 o'clock, and nothing is drawn outside the band or past its ends.
    draw(100);
    for (double deg = -57; deg <= 57; deg += 3) {
        if (fabs(deg) > 52) continue;   // the knob sits on the top end
        assert(at(deg, HT_BRIGHT_R) == swapped(FILL));
        assert(at(deg, HT_BRIGHT_R - HT_BRIGHT_BAND) == 0 && at(deg, HT_BRIGHT_R + HT_BRIGHT_BAND) == 0);     // the band is 10 px wide
    }
    assert(at(0, 100) == 0 && at(180, HT_BRIGHT_R) == 0 && at(90, HT_BRIGHT_R) == 0 && at(-90, HT_BRIGHT_R) == 0 && at(-70, HT_BRIGHT_R) == 0);
    assert(at(-62, HT_BRIGHT_R) == 0 && at(-66, HT_BRIGHT_R) == 0);       // nothing past the bottom end
    assert(at(0, HT_BRIGHT_R - 4) != 0 && at(0, HT_BRIGHT_R + 4) != 0);   // 206..215 about the centre line

    // 8 percent: the knob is at 5 o'clock and the whole arc is track.
    draw(HT_BRIGHT_MIN);
    for (double deg = -50; deg <= 55; deg += 5) assert(at(deg, HT_BRIGHT_R) == swapped(TRACK));
    int kx, ky;
    ht_bright_knob(HT_BRIGHT_MIN, &kx, &ky);
    assert(px[ky * HT_WIDTH + kx] == swapped(KNOB) && ky > 400);

    // 50 percent: the fill runs from the bottom up to the knob, the track shows the rest, the knob is on top of the seam.
    draw(50);
    ht_bright_knob(50, &kx, &ky);
    assert(px[ky * HT_WIDTH + kx] == swapped(KNOB));
    const double knob_deg = atan2(233.0 - ky, kx - 233.0) * 180.0 / 3.14159265358979;
    for (double deg = -55; deg < knob_deg - 10; deg += 3) assert(at(deg, HT_BRIGHT_R) == swapped(FILL));
    for (double deg = knob_deg + 10; deg < 55; deg += 3) assert(at(deg, HT_BRIGHT_R) == swapped(TRACK));

    // Lower percent, shorter fill; the knob follows. Compare how many fill pixels there are at 20, 40, 60, 80 percent.
    long fill_px[4];
    for (int i = 0; i < 4; i++) {
        draw(20 * (i + 1));
        fill_px[i] = 0;
        for (int j = 0; j < HT_WIDTH * HT_HEIGHT; j++) fill_px[i] += px[j] == swapped(FILL);
    }
    assert(fill_px[0] > 0 && fill_px[0] < fill_px[1] && fill_px[1] < fill_px[2] && fill_px[2] < fill_px[3]);

    // The whole arc stays clear of the battery rim (r 226 to 230, 240 to 300 degrees) and of the link icon's place only by design: the icon is
    // not drawn on this screen (ui_habitat.c). What the arc must never do is leave the glass or touch the rim band.
    for (int p = HT_BRIGHT_MIN; p <= HT_BRIGHT_MAX; p += 4) {
        draw(p);
        for (int y = 0; y < HT_HEIGHT; y++)
            for (int x = 0; x < HT_WIDTH; x++)
                if (px[y * HT_WIDTH + x]) assert(hypot(x - 233.0, y - 233.0) < 225.0);
    }
    printf("brightness arc: percent <-> height, touch band, 3 runs, track/fill/knob pixels at 8, 20..100 percent, inside r 225 PASS\n");
    return 0;
}
