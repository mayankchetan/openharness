// Greek pi (U+03C0) in the dial's two font families (dial-reach T12), on a laptop.
//
//   devices/harness-device/firmware/test/run.sh
//
// A pi-engine agent's title starts with "π". Both skins drew "?" for it: the Focus skin's seven Inter faces had no code for it (plookup falls back to '?'),
// and the terminal skin's fixed-cell atlases run from 32 to 255 and map anything else to '?'. The Inter faces now carry the glyph (TEXT in
// gen_lvgl_assets.py); the four Vietnamese atlases carry it as the ninth letter of their tail (VIET_TAIL), at U+1F02, and terminal.c aliases U+03C0 onto it.
#include "../main/ui/habitat/terminal.h"
#include "../main/ui/habitat/focus_faces.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t drawn[HT_WIDTH * HT_HEIGHT], other[HT_WIDTH * HT_HEIGHT];

static int find_code(const ht_pfont_t *face, unsigned code)
{
    int lo = 0, hi = face->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (face->codes[mid] == code) return mid;
        if (face->codes[mid] < code) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void inter_faces(void)
{
    static const struct { const char *name; const ht_pfont_t *face; } faces[] = {
        {"inter_20", &ht_lv_inter_20}, {"inter_25", &ht_lv_inter_25}, {"inter_med_26", &ht_lv_inter_med_26}, {"inter_28", &ht_lv_inter_28},
        {"inter_30", &ht_lv_inter_30}, {"inter_44", &ht_lv_inter_44}, {"inter_36", &ht_lv_inter_36},
    };
    for (unsigned i = 0; i < sizeof faces / sizeof faces[0]; i++) {
        const ht_pfont_t *face = faces[i].face;
        int at = find_code(face, 0x3C0);
        if (at < 0) { printf("FAIL %s has no glyph for U+03C0\n", faces[i].name); assert(0); }
        const ht_glyph_t *g = &face->glyphs[at];
        assert(g->w > 0 && g->h > 0 && g->adv > 0);
        // a 4-bit stream, high nibble first: at least one nonzero nibble
        const size_t bytes = ((size_t)g->w * g->h + 1) / 2;
        int ink = 0;
        for (size_t b = 0; b < bytes; b++) if (face->base.pixels[g->offset + b]) { ink = 1; break; }
        assert(ink);
        // the face is still sorted, and '?' (the fallback) and the Latin and Vietnamese neighbours are all still there
        for (int k = 1; k < face->count; k++) assert(face->codes[k - 1] < face->codes[k]);
        assert(find_code(face, 0x3F) >= 0 && find_code(face, 0x17F) >= 0 && find_code(face, 0x1A0) >= 0 && find_code(face, 0x1EA0) >= 0);
        // exactly one code more than before (423 -> 424)
        assert(face->count == 424);
        // the width is a real width, and not the '?' width
        assert(ht_measure(&face->base, "\xcf\x80") > 0);
        assert(ht_measure(&face->base, "\xcf\x80") != ht_measure(&face->base, "?"));
    }
    // the curved name's face is inter_med_26: the pi is in its measure too
    assert(ht_arc_measure(&ht_arc_inter_prop, "\xcf\x80") > 0);
    assert(ht_arc_measure(&ht_arc_inter_prop, "\xcf\x80") != ht_arc_measure(&ht_arc_inter_prop, "?"));
    assert(ht_arc_measure(&ht_arc_inter_lower, "\xcf\x80") > 0);
}

static void mono_faces(void)
{
    const char *p = "\xcf\x80";
    assert(ht_utf8_next(&p) == 0x3C0 && *p == 0);
    static const struct { const ht_font_t *mono; const ht_font_t *viet; } pairs[] = {
        {&ht_mono_16, &ht_viet_16}, {&ht_mono_20, &ht_viet_20}, {&ht_mono_24, &ht_viet_24}, {&ht_mono_28, &ht_viet_28},
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        // the Vietnamese atlas ends at the ninth tail letter
        assert(pairs[i].viet->first == 0x1ea0 && pairs[i].viet->last == 0x1f02);
        const ht_font_t *font = pairs[i].mono;
        assert(ht_can_display("\xcf\x80", font, font->width, 1));
        ht_scene_t a, b, c;
        ht_scene_clear(&a, 0); ht_scene_clear(&b, 0); ht_scene_clear(&c, 0);
        ht_text(&a, 80, 100, font->width, font, 0xffff, 0, "\xcf\x80");
        assert(!strcmp(a.runs[0].text, "\xcf\x80"));   // the wire text is never rewritten
        ht_text(&b, 80, 100, font->width, font, 0xffff, 0, "?");
        ht_text(&c, 80, 100, font->width, font, 0xffff, 0, " ");
        ht_raster(&a, (ht_rect_t){0, 0, HT_WIDTH, HT_HEIGHT}, drawn);
        ht_raster(&b, (ht_rect_t){0, 0, HT_WIDTH, HT_HEIGHT}, other);
        assert(memcmp(drawn, other, sizeof drawn) != 0);       // not the fallback question mark
        ht_raster(&c, (ht_rect_t){0, 0, HT_WIDTH, HT_HEIGHT}, other);
        assert(memcmp(drawn, other, sizeof drawn) != 0);       // and not an empty cell: it has ink
        // the cell the pi comes from has ink, and the eight letters before it still do
        const ht_font_t *viet = pairs[i].viet;
        const size_t cell = ((size_t)viet->width * viet->height + 3) / 4;   // 2 bits a pixel
        for (unsigned code = 0x1efa; code <= 0x1f02; code++) {
            int ink = 0;
            const uint8_t *px = viet->pixels + (size_t)(code - viet->first) * cell;
            for (size_t k = 0; k < cell; k++) if (px[k]) { ink = 1; break; }
            assert(ink);
        }
    }
    // the curved name's mono face is ht_mono_24: its Vietnamese atlas is the 24 px one, which carries the pi
    assert(ht_arc_geist.viet == &ht_viet_24 && ht_arc_geist.viet->last == 0x1f02);
    assert(ht_arc_measure(&ht_arc_geist, "\xcf\x80") == ht_arc_measure(&ht_arc_geist, "a"));   // one cell, like any letter
    // Greek letters beyond the pi stay '?': only the one letter was added
    assert(!ht_can_display("\xce\xb1", &ht_mono_28, ht_mono_28.width, 1));   // alpha
}

int main(void)
{
    printf("font_pi\n");
    inter_faces();
    mono_faces();
    puts("  pi in the seven Inter faces (424 codes each), the four Vietnamese atlases (tail to U+1F02), the curved faces and both skins' measure PASS");
    return 0;
}
