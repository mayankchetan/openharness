// The permanent link icon (ui/habitat/link_icon.c; wifi-first work, T5, spec section 3.6): a 20 x 20 px mark centred at (379, 379), 4:30 on the round
// glass, shown while a wireless mode runs. Wi-Fi glyph = three ring arcs and a dot (4 runs); phone glyph = a portrait outline, a speaker slot and a
// home dot (3 runs). Normal ink while the session is up, amber (the theme's WARN) while not. No text, no tap target, no heap, no bitmap.
//
// Checked here, with the real compositor (terminal.c) on the host:
//   - the run count, the order (appended last, nothing reordered) and the skip rule (fewer than 4 free runs: nothing added, nothing displaced);
//   - the colours, both glyphs, both states;
//   - the footprint: every lit pixel inside the 20 x 20 box and inside r 221 about the centre (233, 233);
//   - the overlap check of the spec: no pixel shared with the battery rim (track, fill and the percent number, every percent from 0 to 100), with the
//     "Switching in a moment" notice line (Focus face, Inter 25), or with the lower-arc status lines the dial can show (the relaxing lines, the Listening
//     word); and what a status line that fills the whole lower arc does (reported; the spec's gallery check).
#include "../main/ui/habitat/link_icon.h"
#include "../main/ui/habitat/focus_faces.h"
#include "../main/ui/habitat/battery_rim.h"
#include "../main/battery.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t before[HT_WIDTH * HT_HEIGHT], after[HT_WIDTH * HT_HEIGHT];
static const ht_rect_t all = {0, 0, HT_WIDTH, HT_HEIGHT};
enum { NORMAL = 0xADB5, WARN = 0xFD80, GROUND = 0x0841, RIM_NORMAL = 0xADB5, AMBER = 0xFD80, RED = 0xC986, TRACK = 0x2104, TEXT = 0xEF3B, TEXT_BG = 0x0841 };
static const ht_link_icon_ink_t ink = {NORMAL, WARN, GROUND};
static const ht_battery_rim_ink_t rim_ink = {RIM_NORMAL, AMBER, RED, TRACK, TEXT, TEXT_BG};
static uint16_t swapped(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static ht_scene_t icon_scene(ht_link_kind_t kind, bool up)
{
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    assert(ht_link_icon(&s, kind, up, &ink));
    return s;
}
static void render(const ht_scene_t *s, uint16_t *px) { ht_raster(s, all, px); }
static unsigned lit_count(const uint16_t *px)
{
    unsigned n = 0;
    for (int i = 0; i < HT_WIDTH * HT_HEIGHT; i++) n += px[i] != 0;
    return n;
}
// The number of pixels lit in both renders.
static unsigned shared(const uint16_t *a, const uint16_t *b)
{
    unsigned n = 0;
    for (int i = 0; i < HT_WIDTH * HT_HEIGHT; i++) n += a[i] != 0 && b[i] != 0;
    return n;
}

int main(void)
{
    // ── runs, order, skip ─────────────────────────────────────────────────────────────────────────────
    ht_scene_t s;
    ht_scene_clear(&s, 0);
    assert(ht_link_icon(&s, HT_LINK_NONE, true, &ink) && s.count == 0);                         // USB: nothing, and that is no refusal
    assert(ht_link_icon(&s, (ht_link_kind_t)7, true, &ink) && s.count == 0);                    // a number that is no kind: nothing
    ht_text(&s, 10, 10, 100, &ht_mono_16, 0xFFFF, 0, "x");
    assert(ht_link_icon(&s, HT_LINK_WIFI, true, &ink) && s.count == 1 + 4);                     // three arcs and a dot, appended after the screen's own run
    assert(s.runs[0].text[0] == 'x' && s.runs[1].ring.set && s.runs[2].ring.set && s.runs[3].ring.set && !s.runs[4].ring.set && s.runs[4].box.h);
    ht_scene_clear(&s, 0);
    assert(ht_link_icon(&s, HT_LINK_PHONE, true, &ink) && s.count == 3 && s.runs[0].box.h && s.runs[1].box.h && s.runs[2].box.h);
    // fewer than 4 free runs (the rim's own rule): nothing is added and nothing is displaced, for both glyphs
    for (int kind = HT_LINK_WIFI; kind <= HT_LINK_PHONE; kind++) {
        ht_scene_clear(&s, 0);
        while (s.count < HT_RUNS - 3) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
        const ht_scene_t full = s;
        assert(!ht_link_icon(&s, (ht_link_kind_t)kind, true, &ink) && s.count == HT_RUNS - 3 && !memcmp(&s, &full, sizeof s));
        ht_scene_clear(&s, 0);
        while (s.count < HT_RUNS - 4) ht_text(&s, 0, 0, 10, &ht_mono_16, 0xFFFF, 0, "x");
        assert(ht_link_icon(&s, (ht_link_kind_t)kind, true, &ink) && s.count > HT_RUNS - 4 && s.count <= HT_RUNS);
    }
    assert(HT_LINK_ICON_RUNS == 4);

    // ── colours: normal while the session is up, amber while not ──────────────────────────────────────
    ht_scene_t wifi_up = icon_scene(HT_LINK_WIFI, true), wifi_dn = icon_scene(HT_LINK_WIFI, false);
    ht_scene_t phone_up = icon_scene(HT_LINK_PHONE, true), phone_dn = icon_scene(HT_LINK_PHONE, false);
    render(&wifi_up, before);
    assert(before[372 * HT_WIDTH + 379] == swapped(NORMAL));            // the outer arc, straight up from the dot (379, 385): r 13
    assert(before[376 * HT_WIDTH + 379] == swapped(NORMAL));            // the middle arc, r 9
    assert(before[380 * HT_WIDTH + 379] == swapped(NORMAL));            // the inner arc, r 5
    assert(before[385 * HT_WIDTH + 379] == swapped(NORMAL));            // the dot
    render(&wifi_dn, after);
    assert(after[372 * HT_WIDTH + 379] == swapped(WARN) && after[385 * HT_WIDTH + 379] == swapped(WARN));
    render(&phone_up, before);
    assert(before[379 * HT_WIDTH + 373] == swapped(NORMAL) && before[379 * HT_WIDTH + 384] == swapped(NORMAL));   // the outline's sides
    assert(before[379 * HT_WIDTH + 379] == 0 || before[379 * HT_WIDTH + 379] == swapped(GROUND));                  // inside: the ground
    assert(before[373 * HT_WIDTH + 378] == swapped(NORMAL) && before[385 * HT_WIDTH + 378] == swapped(NORMAL));    // the slot and the home dot
    render(&phone_dn, after);
    assert(after[379 * HT_WIDTH + 373] == swapped(WARN) && after[373 * HT_WIDTH + 378] == swapped(WARN));
    // the same shape in both states: only the colour differs
    render(&wifi_up, before);
    render(&wifi_dn, after);
    for (int i = 0; i < HT_WIDTH * HT_HEIGHT; i++) assert((before[i] != 0) == (after[i] != 0));

    // ── footprint: inside the 20 x 20 box and inside r 221 ───────────────────────────────────────────
    const ht_scene_t *glyphs[] = {&wifi_up, &wifi_dn, &phone_up, &phone_dn};
    for (unsigned g = 0; g < 4; g++) {
        render(glyphs[g], before);
        unsigned n = 0;
        for (int y = 0; y < HT_HEIGHT; y++) for (int x = 0; x < HT_WIDTH; x++) {
            if (!before[y * HT_WIDTH + x]) continue;
            n++;
            assert(x >= HT_LINK_ICON_CX - 10 && x < HT_LINK_ICON_CX + 10 && y >= HT_LINK_ICON_CY - 10 && y < HT_LINK_ICON_CY + 10);
            assert(hypot(x + 0.5 - 233.0, y + 0.5 - 233.0) < 221.0);   // 5 px inside the battery band (r 226), 15 degrees clear of its end
        }
        if (n < 40 || n > 300) fprintf(stderr, "glyph %u lit %u\n", g, n);
        assert(n >= 40 && n <= 300);                                     // really drawn, and small (the phone outline also paints its near-black inside)
        for (int i = 0; i < glyphs[g]->count; i++) {                      // the run bounds (what the damage diff uses) hold the same
            const ht_rect_t b = ht_run_bounds(&glyphs[g]->runs[i]);
            assert(b.x >= HT_LINK_ICON_CX - 11 && b.x + b.w <= HT_LINK_ICON_CX + 11 && b.y >= HT_LINK_ICON_CY - 11 && b.y + b.h <= HT_LINK_ICON_CY + 11);   // an arc's bounds are one pixel wider than its ink
        }
    }
    // the damage of a change of state is the icon's box and nothing else
    {
        ht_damage_t d;
        ht_damage(&wifi_up, &wifi_dn, &d);
        assert(d.count >= 1 && d.pixels <= 24 * 24);
        for (int i = 0; i < d.count; i++)
            assert(d.rect[i].x >= 360 && d.rect[i].x + d.rect[i].w <= 400 && d.rect[i].y >= 360 && d.rect[i].y + d.rect[i].h <= 400);
    }

    // ── the overlap check: the battery rim, every percent, with and without the number ──────────────
    unsigned worst_rim = 0;
    for (int pct = 0; pct <= 100; pct++) {
        for (int number = 0; number < 2; number++) {
            ht_scene_t rim;
            ht_scene_clear(&rim, 0);
            assert(ht_battery_rim(&rim, pct, number, &rim_ink));
            render(&rim, before);
            for (unsigned g = 0; g < 4; g++) {
                render(glyphs[g], after);
                const unsigned n = shared(before, after);
                if (n > worst_rim) worst_rim = n;
            }
            // and the number's box (where the number is, the icon is nowhere near)
            if (number) {
                ht_scene_t n = rim;
                const ht_rect_t tb = ht_run_bounds(&n.runs[2]);
                for (int y = tb.y; y < tb.y + tb.h; y++) for (int x = tb.x; x < tb.x + tb.w; x++) assert(!(x >= 369 && x < 389 && y >= 369 && y < 389));
            }
        }
    }
    assert(worst_rim == 0);

    // ── the notice line: "Switching in a moment", Focus face (Inter 25), centred in a 386 px block at y 346 (ui_habitat.c center/text_in) ─────────────────
    {
        ht_scene_t n;
        ht_scene_clear(&n, 0);
        const ht_font_t *f = &ht_lv_inter_25.base;
        const int w = ht_measure(f, "Switching in a moment");
        ht_text(&n, 40 + (386 - w) / 2, 346 + (ht_mono_28.height - f->height) / 2, w, f, 0xFFFF, 0x0001, "Switching in a moment");
        render(&n, before);
        assert(lit_count(before) > 100);
        for (unsigned g = 0; g < 4; g++) { render(glyphs[g], after); assert(shared(before, after) == 0); }
        const ht_rect_t nb = ht_run_bounds(&n.runs[0]);
        printf("  notice line: %d px wide, x %d..%d, y %d..%d; icon x 369..388, y 369..388\n", w, nb.x, nb.x + nb.w, nb.y, nb.y + nb.h);
    }

    // ── the lower arc: the status lines the dial can show ─────────────────────────────────────────────
    {
        static const char *const lines[] = {"Hold the button to talk", "Hold for tabs", "Tap name for panes", "What's next?", "Your move", "Say the word", "Ask away", "Listening"};
        for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            ht_scene_t st;
            ht_scene_clear(&st, 0);
            ht_arc_status_face(&st, 0xFFFF, lines[i], &ht_arc_inter_lower);
            render(&st, before);
            assert(lit_count(before) > 50);
            for (unsigned g = 0; g < 4; g++) { render(glyphs[g], after); assert(shared(before, after) == 0); }
        }
        // A line that fills the arc (the working status, fitted to HT_ARC_SPAN with an ellipsis): measured, not asserted. It is the spec's open question
        // whether the icon may touch a status this long; the icon is drawn last and is 20 px, so it covers a few glyph pixels of it.
        static const char *const longest = "Running the whole test suite now";
        ht_scene_t st;
        ht_scene_clear(&st, 0);
        ht_arc_status_face(&st, 0xFFFF, longest, &ht_arc_inter_lower);
        render(&st, before);
        render(&wifi_up, after);
        printf("  lower arc: a status of %d px (arc span %d px) shares %u px with the Wi-Fi glyph\n", ht_arc_measure(&ht_arc_inter_lower, longest), (int)HT_ARC_SPAN,
               shared(before, after));
        // The longest prefix of that sentence that still clears the icon (both glyphs): the threshold the bench can read against.
        int clear_px = 0;
        for (size_t n = 1; n <= strlen(longest); n++) {
            char part[64];
            snprintf(part, sizeof part, "%.*s", (int)n, longest);
            ht_scene_clear(&st, 0);
            ht_arc_status_face(&st, 0xFFFF, part, &ht_arc_inter_lower);
            render(&st, before);
            render(&wifi_up, after);
            const unsigned a = shared(before, after);
            render(&phone_up, after);
            if (a == 0 && shared(before, after) == 0) clear_px = ht_arc_measure(&ht_arc_inter_lower, part);
        }
        printf("  lower arc: statuses up to %d px of the %d px span clear the icon (longer ones touch a few glyph pixels of it; the icon is drawn last)\n", clear_px, (int)HT_ARC_SPAN);
        assert(clear_px >= ht_arc_measure(&ht_arc_inter_lower, "Hold the button to talk"));
    }
    puts("test_link_icon: PASS");
    return 0;
}
