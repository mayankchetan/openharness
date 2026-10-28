// The renderer's UI metrics (DEVICE_UI_METRICS): what ht_metrics_scene() and ht_raster() count for the per-view
// UIM line. Compile with -DDEVICE_UI_METRICS=1 against terminal.c and fonts.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "terminal.h"

static uint16_t out[HT_WIDTH * HT_HEIGHT];
static const ht_rect_t whole = {0, 0, HT_WIDTH, HT_HEIGHT};
// The glyph cache serves only 32..126 fonts at most 5 x 10 px (the character art's tiny faces); no daily-UI font
// qualifies, so the test brings one: 4 x 8 px, two bits a pixel, 95 glyphs of 8 bytes.
static uint8_t tiny_pixels[95 * 8];
static const ht_font_t tiny = {32, 126, 4, 8, tiny_pixels};

int main(void)
{
    ht_metrics_t m;
    ht_metrics_take(&m);   // start from zero
    assert(m.arc_cap == 9216 && !m.arc_mask_max[0] && !m.arc_mask_max[1] && !m.cols_max && !m.glyph_hits && !m.glyph_miss);

    // Small-ASCII text in a cacheable font: the first raster builds glyphs (misses), the second finds them (hits).
    // A 20 px mono run is not cacheable and counts nothing.
    ht_scene_t s; ht_scene_clear(&s, 0);
    assert(ht_text(&s, 40, 100, 300, &ht_mono_20, 0xffff, 0, "hello hello"));
    ht_raster(&s, whole, out);
    ht_metrics_take(&m);
    assert(!m.glyph_hits && !m.glyph_miss);
    assert(ht_text(&s, 40, 200, 300, &tiny, 0xffff, 0, "hello hello"));
    ht_raster(&s, whole, out);
    ht_metrics_take(&m);
    assert(m.glyph_miss > 0 && m.glyph_miss <= 24 && m.glyph_hits > 0);   // 'h','e','l','o',' ' repeat inside the text
    unsigned first = m.glyph_miss + m.glyph_hits;
    ht_raster(&s, whole, out);
    ht_metrics_take(&m);
    assert(m.glyph_miss == 0 && m.glyph_hits == first);
    ht_metrics_take(&m);
    assert(!m.glyph_hits && !m.glyph_miss);   // take clears

    // An upper-arc and a lower-arc label: nothing is known before the first raster (the caches hold other text), then
    // the raster reports the mask each really used, within the cap; the upper and lower counters are separate.
    ht_scene_clear(&s, 0);
    ht_arc_title(&s, 0xffff, "A long agent name");
    ht_arc_status(&s, 0xffff, "Listening");
    ht_metrics_scene(&s);
    ht_metrics_take(&m);
    assert(!m.arc_mask_max[0] && !m.arc_mask_max[1]);
    ht_raster(&s, whole, out);
    ht_metrics_take(&m);
    assert(m.arc_mask_max[0] > 0 && m.arc_mask_max[0] <= m.arc_cap);
    assert(m.arc_mask_max[1] > 0 && m.arc_mask_max[1] <= m.arc_cap);
    assert(m.arc_mask_max[0] != m.arc_mask_max[1]);   // different texts, different masks
    // The caches now hold this scene's text, so a later scene with the same labels is measured without a raster
    // (a view whose arcs were not damaged still reports them).
    unsigned up = m.arc_mask_max[0], low = m.arc_mask_max[1];
    ht_metrics_scene(&s);
    ht_metrics_take(&m);
    assert(m.arc_mask_max[0] == up && m.arc_mask_max[1] == low);
    // Other text on the upper arc: the scan does not count the stale mask.
    ht_scene_clear(&s, 0);
    ht_arc_title(&s, 0xffff, "Another");
    ht_metrics_scene(&s);
    ht_metrics_take(&m);
    assert(!m.arc_mask_max[0]);

    // Sprites: the widest row is counted from the scene alone (plain and zoomed), and again when rastered.
    static const uint16_t pal[2] = {0, 0xffff};
    static uint8_t cells[60 * 5];
    memset(cells, 1, sizeof cells);
    ht_cell_frame_t wide = {60, 5, 2, pal, cells, NULL}, narrow = {20, 5, 2, pal, cells, NULL};
    ht_scene_clear(&s, 0);
    assert(ht_cell_sprite(&s, 100, 100, &narrow) && ht_cell_sprite_zoom(&s, 100, 200, &wide, 4));
    ht_metrics_scene(&s);
    ht_metrics_take(&m);
    assert(m.cols_max == 60);   // the zoomed one's source columns, not its glass width
    ht_raster(&s, whole, out);
    ht_metrics_take(&m);
    assert(m.cols_max == 60);

    puts("terminal metrics: glyph hits and misses, upper/lower arc masks (scene scan and raster), sprite columns, take clears PASS");
    return 0;
}
