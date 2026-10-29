// uim.c (DEVICE_UI_METRICS): per-view maxima, the one-line-per-tick rate limit, the 10 s line, the ring and the
// 512 B log-line limit. The IDF calls are fakes (test/host_stubs); terminal.c is the real renderer.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "terminal.h"
#include "uim.h"

static int64_t clock_us;
static unsigned stack_free = 5000;
int64_t esp_timer_get_time(void) { return clock_us; }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 111111; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 65536; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return stack_free; }

static char lines[40][600];
static int line_count, warn_count;
void host_log_info(const char *tag, const char *fmt, ...)
{
    assert(!strcmp(tag, "uim") && line_count < 40);
    va_list ap; va_start(ap, fmt);
    vsnprintf(lines[line_count++], sizeof lines[0], fmt, ap);
    va_end(ap);
}
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; warn_count++; }

static uint16_t out[HT_WIDTH * HT_HEIGHT];
static const ht_rect_t whole = {0, 0, HT_WIDTH, HT_HEIGHT};

static unsigned field(const char *line, const char *key)
{
    char pattern[40];
    snprintf(pattern, sizeof pattern, " %s=", key);
    const char *at = strstr(line, pattern);
    assert(at);
    return (unsigned)strtoul(at + strlen(pattern), NULL, 10);
}

int main(void)
{
    // A home-like scene: an upper arc, a lower arc and two text runs.
    ht_scene_t home, reader;
    ht_scene_clear(&home, 0);
    ht_arc_title(&home, 0xffff, "Agent name");
    ht_arc_status(&home, 0xffff, "Listening");
    ht_text(&home, 40, 200, 300, &ht_mono_20, 0xffff, 0, "one");
    ht_text(&home, 40, 240, 300, &ht_mono_20, 0xffff, 0, "two");
    ht_scene_clear(&reader, 0);
    for (int i = 0; i < 5; i++) ht_text(&reader, 40, 100 + 30 * i, 300, &ht_mono_20, 0xffff, 0, "row");

    // Frame 1 of "home": scene taken, then rasterized and painted. Nothing is logged until the tick finds a reason.
    clock_us = 1000 * 1000;
    uim_scene("home", &home);
    ht_raster(&home, whole, out);
    uim_paint(1200, 5000);
    uim_scene("home", &home);        // another scene of the same view
    ht_raster(&home, whole, out);
    uim_paint(3400, 4000);
    uim_tick();
    assert(line_count == 0);         // 0 s into the view: no end line, no 10 s line yet

    // 10 s into the view: one periodic line with the maxima so far, once.
    clock_us += 10 * 1000 * 1000;
    uim_tick();
    assert(line_count == 1);
    const char *l = lines[0];
    assert(!strncmp(l, "UIM view=home n=2 ", 18));
    assert(field(l, "runs_max") == home.count && field(l, "runs_cap") == HT_RUNS);
    assert(field(l, "arc0_mask_max") > 0 && field(l, "arc1_mask_max") > 0 && field(l, "arc_cap") == 9216);
    assert(field(l, "raster_max_us") == 3400 && field(l, "frame_max_us") == 5000);
    assert(field(l, "render_stack_free_min") == 5000 && field(l, "int_free") == 111111 && field(l, "int_largest") == 65536);
    assert(field(l, "glyph_hits") == 0 && field(l, "glyph_miss") == 0 && field(l, "unpacked_cols_max") == 0);
    assert(strstr(l, "kind=10s"));
    uim_tick();
    assert(line_count == 1);         // the periodic timer restarted

    // The view changes: the finished view is queued; the next tick writes exactly one "end" line for it.
    stack_free = 4200;
    uim_scene("reader", &reader);
    ht_raster(&reader, whole, out);
    uim_paint(800, 2500);
    assert(line_count == 1);         // nothing is logged on the render side
    uim_tick();
    assert(line_count == 2 && !strncmp(lines[1], "UIM view=home n=2 ", 18) && strstr(lines[1], "kind=end"));
    uim_tick();
    assert(line_count == 2);         // the reader view is 0 s old
    // Reader maxima start from zero: its runs and arcs are its own (no arc is on screen).
    clock_us += 10 * 1000 * 1000;
    uim_tick();
    assert(line_count == 3 && !strncmp(lines[2], "UIM view=reader n=1 ", 20));
    assert(field(lines[2], "runs_max") == reader.count && field(lines[2], "arc0_mask_max") == 0);
    assert(field(lines[2], "raster_max_us") == 800 && field(lines[2], "render_stack_free_min") == 4200);

    // A view whose arcs are not damaged is never rastered, yet it shows the arcs: the scene scan reads the masks the
    // arc caches still hold for the same text (no uim_paint here).
    uim_scene("home2", &home);
    uim_tick();                      // the reader view's end line
    clock_us += 10 * 1000 * 1000;
    uim_tick();
    assert(line_count == 5 && !strncmp(lines[4], "UIM view=home2 n=0 ", 19));
    assert(field(lines[4], "arc0_mask_max") > 0 && field(lines[4], "arc1_mask_max") > 0);

    // A burst of view changes between two ticks: the ring holds 8, the tick writes one line a second and says how
    // many views it lost.
    static const char *names[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    for (unsigned i = 0; i < 12; i++) { uim_scene(names[i], &reader); uim_paint(1, 1); }
    int before = line_count;
    uim_tick();
    assert(line_count == before + 1 && warn_count == 1);   // 12 changes queued 12 views, 8 fit
    for (int i = 0; i < 7; i++) uim_tick();
    assert(line_count == before + 8);
    uim_tick();
    assert(line_count == before + 8);                       // ring empty, the current view is fresh

    // The gallery's items: an item names its own record whatever view the scenes come from, closes at once with one
    // synchronous "item" line, and nothing is recorded between items (the setup scenes of the next item are not a view).
    int mark = line_count;
    uim_item_begin("idle_claude");
    uim_scene("home", &home);
    ht_raster(&home, whole, out);
    uim_paint(700, 1500);
    uim_scene("brand", &reader);     // a second scene during the item still belongs to it
    uim_paint(900, 1200);
    assert(line_count == mark);      // nothing yet
    char drawn[20];
    uim_item_scene(drawn, sizeof drawn);
    assert(!strcmp(drawn, "brand"));
    uim_item_end();
    assert(line_count == mark + 1 && !strncmp(lines[mark], "UIM view=idle_claude n=2 ", 25) && strstr(lines[mark], "kind=item"));
    assert(strstr(lines[mark], " scene=brand"));   // what the renderer last built for the item (the second scene), not the item's name
    assert(field(lines[mark], "raster_max_us") == 900 && field(lines[mark], "arc0_mask_max") > 0);
    uim_scene("home", &home);        // between items: ignored, and the counters are discarded
    uim_paint(5, 5);
    uim_tick();
    assert(line_count == mark + 1);
    uim_item_begin("reader_p1");
    uim_scene("reader", &reader);
    uim_paint(100, 200);
    uim_item_end();
    assert(line_count == mark + 2 && !strncmp(lines[mark + 1], "UIM view=reader_p1 n=1 ", 23));
    assert(field(lines[mark + 1], "arc0_mask_max") == 0 && field(lines[mark + 1], "raster_max_us") == 100);

    // Runs the builders refused are counted per view. The model builds the scene first and calls uim_scene() after it,
    // so the refusals belong to the view of that new scene, not to the view it replaces. The next view starts at zero.
    int omark = line_count;
    uim_item_begin("overflow");
    ht_scene_t full;
    ht_scene_clear(&full, 0);
    for (int i = 0; i < HT_RUNS + 2; i++) ht_box(&full, 0, i, 10, 1, 0, 0xffff, 0);   // the last two do not fit
    uim_scene("home", &full);
    uim_paint(1, 1);
    uim_item_end();
    assert(line_count == omark + 1 && field(lines[omark], "runs_drop") == 2 && field(lines[omark], "runs_max") == HT_RUNS);
    uim_item_begin("fits");
    uim_scene("home", &home);
    uim_paint(1, 1);
    uim_item_end();
    assert(line_count == omark + 2 && field(lines[omark + 1], "runs_drop") == 0);

    // The 512 B limit (cable_link.c): the longest record the formatter can make.
    uim_record_t big;
    memset(&big, 0xff, sizeof big);
    memset(big.view, 'v', sizeof big.view - 1);
    big.view[sizeof big.view - 1] = 0;
    memset(big.scene, 's', sizeof big.scene - 1);
    big.scene[sizeof big.scene - 1] = 0;
    char buf[1024];
    int n = uim_format(buf, sizeof buf, &big, "item", UINT32_MAX, UINT32_MAX);   // the longest kind: it carries scene=
    assert(n > 0 && n < 400 && (size_t)n == strlen(buf));
    for (int i = 0; i < line_count; i++) assert(strlen(lines[i]) < 400);

    puts("ui metrics: per-view maxima, 10 s and end lines, one line a tick, ring loss, line under 400 B PASS");
    return 0;
}
