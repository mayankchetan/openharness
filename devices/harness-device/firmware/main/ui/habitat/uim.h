#pragma once
// Per-view UI measurements (DEVICE_UI_METRICS, off by default; compiled only then). One "UIM view=..." log line
// when the view changes and every 10 s while one is shown, so a screen tour on the dial reads off how much of each
// renderer limit (runs per scene, arc mask bytes, sprite columns) the screen really uses.
//
// Threads: uim_scene and uim_paint run on the render task only. uim_tick runs on ONE background task (the refresh
// task's 1 s loop), takes the heap walk and the log call away from the renderer, and writes at most one line a
// second. The two sides share a few counters under a spinlock held for a struct copy.
#include <stddef.h>
#include <stdint.h>
#include "terminal.h"

// Called on the render task, display lock held, right after habitat_scene_take() built a fresh scene.
void uim_scene(const char *view, const ht_scene_t *scene);
// Called on the render task after a frame was painted: raster time (sum of the strips) and whole-frame time, in us.
void uim_paint(uint32_t raster_us, uint32_t frame_us);
// Called about once a second from one background task.
void uim_tick(void);

// The UI gallery (DEVICE_UI_GALLERY): one record per tour item, named for the item whatever views its scenes report.
// uim_item_end() writes the item's line (kind=item) at once on the caller's task. Between items nothing is recorded.
void uim_item_begin(const char *name);
void uim_item_end(void);

// The name of the view the model is on (ui_habitat.c): the view_t enum, with the home face split by what it shows.
const char *habitat_view_name(void);

// "UIM ..." line for one record. Exposed for the host test; `buf` of 512 bytes is always enough.
typedef struct {
    char view[20];
    uint32_t n, runs_max, arc_mask_max[2], glyph_hits, glyph_miss, cols_max, raster_max_us, frame_max_us;
    uint32_t stack_free_min, arc_cap;
} uim_record_t;
int uim_format(char *buf, size_t size, const uim_record_t *r, const char *kind, uint32_t int_free, uint32_t int_largest);
