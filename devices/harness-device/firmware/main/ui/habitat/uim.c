#include "uim.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uim";

enum { RING = 8, PERIODIC_MS = 10000 };

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uim_record_t s_cur;         // the view on screen; written by the render task
static bool s_started;
static char s_item[20];            // the gallery item being shown: names the record whatever the scenes' views are
static volatile bool s_item_active, s_item_mode;   // mode: items are in use, so nothing is recorded between them
static uint32_t s_cur_log_ms;      // when the current view was last logged (render task sets it at the start, tick after)
static uim_record_t s_ring[RING];  // finished views waiting for the tick
static unsigned s_ring_head, s_ring_count, s_ring_lost;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void maxu(uint32_t *to, uint32_t v) { if (v > *to) *to = v; }

// Fold counters the renderer took (ht_metrics_take) into the current view. Caller holds s_lock.
static void fold(const ht_metrics_t *m)
{
    maxu(&s_cur.arc_mask_max[0], m->arc_mask_max[0]);
    maxu(&s_cur.arc_mask_max[1], m->arc_mask_max[1]);
    maxu(&s_cur.cols_max, m->cols_max);
    s_cur.glyph_hits += m->glyph_hits;
    s_cur.glyph_miss += m->glyph_miss;
    s_cur.runs_drop += m->runs_dropped;
    s_cur.arc_cap = m->arc_cap;
}

void uim_scene(const char *view, const ht_scene_t *scene)
{
    uint32_t stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);   // bytes, the render task's lowest free so far
    uint32_t now = now_ms();
    if (s_item_mode && !s_item_active) {   // between gallery items: the setup scenes are not a view
        ht_metrics_t dropped;
        ht_metrics_take(&dropped);
        return;
    }
    // The counters belong to the render task, so they are read outside the lock. What a screenshot raster counted
    // since the last frame belongs to the view that was on screen; what the new scene shows belongs to the new one.
    ht_metrics_t before, shown;
    ht_metrics_take(&before);
    // The model built this scene just before this call, so the runs its builders refused belong to the new view.
    const uint32_t refused = before.runs_dropped;
    before.runs_dropped = 0;
    ht_metrics_scene(scene);
    ht_metrics_take(&shown);
    portENTER_CRITICAL(&s_lock);
    fold(&before);
    const char *built = view;       // what the model says it built this scene for
    if (s_item[0]) view = s_item;   // an item names its record
    if (!s_started || strncmp(s_cur.view, view, sizeof s_cur.view - 1) != 0) {
        if (s_started) {
            s_ring[(s_ring_head + s_ring_count) % RING] = s_cur;
            if (s_ring_count < RING) s_ring_count++;
            else { s_ring_head = (s_ring_head + 1) % RING; s_ring_lost++; }
        }
        memset(&s_cur, 0, sizeof s_cur);
        snprintf(s_cur.view, sizeof s_cur.view, "%s", view);
        s_started = true;
        s_cur_log_ms = now;
    }
    fold(&shown);
    s_cur.runs_drop += refused;
    snprintf(s_cur.scene, sizeof s_cur.scene, "%s", built);
    maxu(&s_cur.runs_max, scene->count);
    s_cur.stack_free_min = stack;
    portEXIT_CRITICAL(&s_lock);
}

void uim_paint(uint32_t raster_us, uint32_t frame_us)
{
    uint32_t stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ht_metrics_t m;
    ht_metrics_take(&m);
    if (s_item_mode && !s_item_active) return;
    portENTER_CRITICAL(&s_lock);
    fold(&m);
    s_cur.n++;
    maxu(&s_cur.raster_max_us, raster_us);
    maxu(&s_cur.frame_max_us, frame_us);
    s_cur.stack_free_min = stack;
    portEXIT_CRITICAL(&s_lock);
}

int uim_format(char *buf, size_t size, const uim_record_t *r, const char *kind, uint32_t int_free, uint32_t int_largest)
{
    return snprintf(buf, size,
        "UIM view=%s n=%lu runs_max=%lu runs_cap=%u runs_drop=%lu arc0_mask_max=%lu arc1_mask_max=%lu arc_cap=%lu "
        "glyph_hits=%lu glyph_miss=%lu unpacked_cols_max=%lu raster_max_us=%lu frame_max_us=%lu "
        "render_stack_free_min=%lu int_free=%lu int_largest=%lu kind=%s%s%s",
        r->view, (unsigned long)r->n, (unsigned long)r->runs_max, (unsigned)HT_RUNS, (unsigned long)r->runs_drop,
        (unsigned long)r->arc_mask_max[0], (unsigned long)r->arc_mask_max[1], (unsigned long)r->arc_cap,
        (unsigned long)r->glyph_hits, (unsigned long)r->glyph_miss, (unsigned long)r->cols_max,
        (unsigned long)r->raster_max_us, (unsigned long)r->frame_max_us,
        (unsigned long)r->stack_free_min, (unsigned long)int_free, (unsigned long)int_largest, kind,
        !strcmp(kind, "item") ? " scene=" : "", !strcmp(kind, "item") ? r->scene : "");
}

static void emit(const uim_record_t *r, const char *kind)
{
    char line[400];   // the longest record is 397 B with every field at its maximum (test_uim.c)
    // int_free is cheap; int_largest walks every region's free list under the heap lock, which is why this runs here
    // and not on the render task. Both are read when the line is written, up to a second after a view ended.
    uim_format(line, sizeof line, r, kind,
               (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "%s", line);
}

void uim_tick(void)
{
    uim_record_t record;
    memset(&record, 0, sizeof record);
    const char *kind = NULL;
    unsigned lost = 0;
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    if (s_ring_count) {
        record = s_ring[s_ring_head];
        s_ring_head = (s_ring_head + 1) % RING;
        s_ring_count--;
        kind = "end";
    } else if (s_started && now - s_cur_log_ms >= PERIODIC_MS) {
        record = s_cur;
        s_cur_log_ms = now;
        kind = "10s";
    }
    lost = s_ring_lost;
    s_ring_lost = 0;
    portEXIT_CRITICAL(&s_lock);
    if (lost) ESP_LOGW(TAG, "UIM lost=%u views (more than %d changes in a second)", lost, RING);
    if (kind) emit(&record, kind);
}

void uim_item_begin(const char *name)
{
    portENTER_CRITICAL(&s_lock);
    s_ring_count = 0;   // gallery images have no tick; whatever view was recorded before the first item is not wanted
    memset(&s_cur, 0, sizeof s_cur);
    snprintf(s_cur.view, sizeof s_cur.view, "%s", name);
    snprintf(s_item, sizeof s_item, "%s", name);
    s_started = true;
    s_item_mode = true;
    s_item_active = true;
    s_cur_log_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);
}

void uim_item_end(void)
{
    uim_record_t record;
    portENTER_CRITICAL(&s_lock);
    record = s_cur;
    s_item_active = false;
    s_item[0] = 0;
    s_started = false;
    portEXIT_CRITICAL(&s_lock);
    emit(&record, "item");   // here and now, on the caller's task: the gallery has no tick
}

void uim_item_scene(char *out, size_t size)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(out, size, "%s", s_cur.scene);
    portEXIT_CRITICAL(&s_lock);
}
