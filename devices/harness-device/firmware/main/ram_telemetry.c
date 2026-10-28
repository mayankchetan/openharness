#include "ram_telemetry.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/display.h"

#if !CONFIG_IDF_TARGET_ESP32
#include "esp_private/flash_mmap.h"
#endif
#ifdef DEVICE_RAM_REGIONS
// registered_heaps: the per-region heap handles, for min_free per region. heap_caps_get_info() only sums them.
// The include path is added in main/CMakeLists.txt under the same flag. The heap component defines
// MULTI_HEAP_FREERTOS for its own sources (it selects the portMUX lock type in heap_t); repeat that here.
#define MULTI_HEAP_FREERTOS 1
#include "heap_private.h"
#endif

static const char *TAG = "ram";

#define APP_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_PSRAM_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static bool s_initialized;
static volatile uint32_t s_json_fallback_count;
static volatile uint64_t s_json_fallback_bytes;
static volatile uint32_t s_psram_failure_count;
static volatile uint64_t s_psram_failure_bytes;
static uint32_t s_periodic_count;
static volatile int s_mmap_remain_cached = -1;   // -1 = not sampled yet
static int64_t s_mmap_next_us;                   // background task only
#ifdef DEVICE_RAM_REGIONS
static int64_t s_regions_next_us = 61LL * 1000000LL;   // first t5min line 61 s after boot; background task only
static void alloc_failed_cb(size_t size, uint32_t caps, const char *function_name);
#endif

static void note_psram_failure(const char *owner, size_t bytes)
{
    __atomic_fetch_add(&s_psram_failure_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_psram_failure_bytes, (uint64_t)bytes, __ATOMIC_RELAXED);
    ESP_LOGE(TAG, "PSRAM allocation failed owner=%s bytes=%u",
             owner ? owner : "unknown", (unsigned)bytes);
}

static void *cjson_psram_malloc(size_t bytes)
{
    void *ptr = heap_caps_malloc(bytes, APP_PSRAM_CAPS);
    if (ptr) return ptr;

    __atomic_fetch_add(&s_json_fallback_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_json_fallback_bytes, (uint64_t)bytes, __ATOMIC_RELAXED);
    return heap_caps_malloc(bytes, APP_INTERNAL_CAPS);
}

static void cjson_heap_free(void *ptr)
{
    free(ptr);
}

void ram_telemetry_init(void)
{
    if (s_initialized) return;
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn = cjson_heap_free,
    };
    cJSON_InitHooks(&hooks);
#ifdef DEVICE_RAM_REGIONS
    heap_caps_register_failed_alloc_callback(alloc_failed_cb);
#endif
    s_initialized = true;
}


void *ram_psram_alloc(size_t bytes, const char *owner)
{
    void *ptr = heap_caps_malloc(bytes, APP_PSRAM_CAPS);
    if (!ptr && bytes) note_psram_failure(owner, bytes);
    return ptr;
}

void *ram_psram_calloc(size_t count, size_t size, const char *owner)
{
    if (count && size > SIZE_MAX / count) {
        note_psram_failure(owner, SIZE_MAX);
        return NULL;
    }
    void *ptr = heap_caps_calloc(count, size, APP_PSRAM_CAPS);
    if (!ptr && count && size) note_psram_failure(owner, count * size);
    return ptr;
}

void *ram_psram_realloc(void *ptr, size_t bytes, const char *owner)
{
    void *next = heap_caps_realloc(ptr, bytes, APP_PSRAM_CAPS);
    if (!next && bytes) note_psram_failure(owner, bytes);
    return next;
}

char *ram_psram_strdup(const char *src, const char *owner)
{
    if (!src) return NULL;
    size_t bytes = strlen(src) + 1;
    char *dst = ram_psram_alloc(bytes, owner);
    if (dst) memcpy(dst, src, bytes);
    return dst;
}

// The ten application tasks (names as passed to xTaskCreate*; checked by grep in the T1 commit message).
static const char *const k_hwm_tasks[] = {
    "habitat_render",   // ui/habitat/display_habitat.c
    "habitat_touch",    // ui/habitat/touch_habitat.c
    "habitat_actions",  // ui/habitat/ui_habitat.c
    "cable_link",       // cable_link.c
    "cable_session",    // cable_client.c
    "voice_capture",    // audio_client_habitat.c
    "voice_send",       // audio_client_habitat.c
    "ptt",              // ptt.c
    "beep",             // audio_capture.c
    "proj_refresh",     // app_main.c
};

static void log_task_stack(const char *name)
{
    TaskHandle_t task = xTaskGetHandle(name);
    if (!task) return;
    ESP_LOGI(TAG, "STACK task=%s free_min=%u B",
             name, (unsigned)uxTaskGetStackHighWaterMark(task));
}

static bool tag_wants_regions(const char *tag)
{
    static const char *const tags[] = {"boot", "ui_ready", "voice_buffer_ready", "app_ready"};
    if (!tag) return false;
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (strcmp(tag, tags[i]) == 0) return true;
    return false;
}

void ram_telemetry_checkpoint(const char *tag)
{
    uint32_t json_fallbacks = __atomic_load_n(&s_json_fallback_count, __ATOMIC_RELAXED);
    uint64_t json_fallback_bytes = __atomic_load_n(&s_json_fallback_bytes, __ATOMIC_RELAXED);
    uint32_t psram_failures = __atomic_load_n(&s_psram_failure_count, __ATOMIC_RELAXED);
    uint64_t psram_failure_bytes = __atomic_load_n(&s_psram_failure_bytes, __ATOMIC_RELAXED);

    ESP_LOGI(TAG,
             "RAM tag=%s int_free=%u int_min=%u int_largest=%u psram_free=%u psram_largest=%u "
             "json_fb=%u/%lluB psram_fail=%u/%lluB task=%s stack_free_min=%uB",
             tag ? tag : "unknown",
             (unsigned)heap_caps_get_free_size(APP_INTERNAL_CAPS),
             (unsigned)heap_caps_get_minimum_free_size(APP_INTERNAL_CAPS),
             (unsigned)heap_caps_get_largest_free_block(APP_INTERNAL_CAPS),
             (unsigned)heap_caps_get_free_size(APP_PSRAM_CAPS),
             (unsigned)heap_caps_get_largest_free_block(APP_PSRAM_CAPS),
             (unsigned)json_fallbacks,
             (unsigned long long)json_fallback_bytes,
             (unsigned)psram_failures,
             (unsigned long long)psram_failure_bytes,
             pcTaskGetName(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (tag_wants_regions(tag)) ram_telemetry_regions(tag);
    ram_telemetry_alloc_fail_flush();
}

void ram_telemetry_periodic(const char *tag)
{
    ram_telemetry_checkpoint(tag);
    if (++s_periodic_count % 10 != 0) return;

    for (size_t i = 0; i < sizeof(k_hwm_tasks) / sizeof(k_hwm_tasks[0]); i++) log_task_stack(k_hwm_tasks[i]);
}

void ram_telemetry_hwm(void)
{
    char line[320];
    size_t n = 0;
    line[0] = 0;
    for (size_t i = 0; i < sizeof(k_hwm_tasks) / sizeof(k_hwm_tasks[0]); i++) {
        TaskHandle_t task = xTaskGetHandle(k_hwm_tasks[i]);
        int len = snprintf(line + n, sizeof(line) - n, " %s=%d", k_hwm_tasks[i],
                           task ? (int)uxTaskGetStackHighWaterMark(task) : -1);
        if (len < 0 || (size_t)len >= sizeof(line) - n) break;
        n += (size_t)len;
    }
    ESP_LOGI(TAG, "HWM up=%llus%s", (unsigned long long)(esp_timer_get_time() / 1000000), line);
}

bool ram_flash_mmap_remain(void)
{
#if !CONFIG_IDF_TARGET_ESP32
    flash_mmap_lock_freeze();
    bool remain = flash_mmap_remain();
    flash_mmap_lock_unfreeze();
    return remain;
#else
    return false;
#endif
}

int ram_flash_mmap_remain_cached(void)
{
    return __atomic_load_n(&s_mmap_remain_cached, __ATOMIC_RELAXED);
}

void ram_telemetry_background_tick(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us >= s_mmap_next_us) {
        s_mmap_next_us = now_us + 10LL * 1000000LL;
        // May wait for another task's flash write or mapping; that is why this runs here and not in the renderer.
        __atomic_store_n(&s_mmap_remain_cached, ram_flash_mmap_remain() ? 1 : 0, __ATOMIC_RELAXED);
        now_us = esp_timer_get_time();
    }
#ifdef DEVICE_RAM_REGIONS
    if (now_us >= s_regions_next_us) {
        s_regions_next_us += 300LL * 1000000LL;
        if (s_regions_next_us <= now_us) s_regions_next_us = now_us + 300LL * 1000000LL;   // never burst after a stall
        ram_telemetry_regions("t5min");
    }
#endif
    ram_telemetry_alloc_fail_flush();
}

#ifdef DEVICE_RAM_REGIONS

int ram_telemetry_region_snapshot(ram_region_t *out, int max)
{
    int n = 0;
    heap_t *heap;
    SLIST_FOREACH(heap, &registered_heaps, next) {
        if (!heap_caps_match(heap, MALLOC_CAP_INTERNAL)) continue;
        if (out && n < max) {
            multi_heap_info_t info;
            multi_heap_get_info(heap->heap, &info);
            out[n].start = (uintptr_t)heap->start;
            out[n].size = (uint32_t)(heap->end - heap->start);
            out[n].free_b = (uint32_t)info.total_free_bytes;
            out[n].largest = (uint32_t)info.largest_free_block;
            out[n].min_free = (uint32_t)info.minimum_free_bytes;
        }
        n++;
    }
    return n;
}

void ram_telemetry_regions(const char *tag)
{
    ram_region_t regions[16];
    int n = ram_telemetry_region_snapshot(regions, 16);
    for (int i = 0; i < n && i < 16; i++) {
        ESP_LOGI(TAG, "RAM_REGIONS tag=%s idx=%d/%d start=0x%08x size=%u free=%u largest=%u min_free=%u",
                 tag ? tag : "unknown", i, n, (unsigned)regions[i].start, (unsigned)regions[i].size,
                 (unsigned)regions[i].free_b, (unsigned)regions[i].largest, (unsigned)regions[i].min_free);
    }
}

// Failed-allocation record. The IDF hook (heap_caps.c heap_caps_alloc_failed) can run from any task, so it
// only stores; logging waits for ram_telemetry_alloc_fail_flush().
#define AF_RING 8
#define AF_LOG_PER_FLUSH 4
typedef struct {
    uint32_t size, caps, t_ms;
    const char *fn;
} af_rec_t;
static af_rec_t s_af[AF_RING];
static volatile uint32_t s_af_count;
static volatile uint64_t s_af_bytes;
static uint32_t s_af_reported;
static uint32_t s_af_last_flush_ms;

static void alloc_failed_cb(size_t size, uint32_t caps, const char *function_name)
{
    uint32_t seq = __atomic_fetch_add(&s_af_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_af_bytes, (uint64_t)size, __ATOMIC_RELAXED);
    af_rec_t *r = &s_af[seq % AF_RING];
    r->size = (uint32_t)size;
    r->caps = caps;
    r->fn = function_name;
    r->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

unsigned ram_telemetry_alloc_fail_count(void)
{
    return __atomic_load_n(&s_af_count, __ATOMIC_RELAXED);
}

void ram_telemetry_alloc_fail_flush(void)
{
    uint32_t total = __atomic_load_n(&s_af_count, __ATOMIC_RELAXED);
    if (total == s_af_reported) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_af_last_flush_ms && now_ms - s_af_last_flush_ms < 1000) return;   // rate limit: one flush per second
    s_af_last_flush_ms = now_ms ? now_ms : 1;

    uint32_t first = s_af_reported;
    uint32_t lost = 0;
    if (total - first > AF_RING) {
        lost = total - first - AF_RING;
        first = total - AF_RING;
    }
    uint32_t last = first + AF_LOG_PER_FLUSH;
    if (last > total) last = total;
    for (uint32_t seq = first; seq < last; seq++) {
        const af_rec_t *r = &s_af[seq % AF_RING];
        ESP_LOGW(TAG, "ALLOC_FAIL seq=%u t_ms=%u size=%u caps=0x%08x fn=%s total=%u bytes=%llu",
                 (unsigned)seq, (unsigned)r->t_ms, (unsigned)r->size, (unsigned)r->caps,
                 r->fn ? r->fn : "?", (unsigned)total,
                 (unsigned long long)__atomic_load_n(&s_af_bytes, __ATOMIC_RELAXED));
    }
    if (lost) ESP_LOGW(TAG, "ALLOC_FAIL overwritten=%u (ring of %d)", (unsigned)lost, AF_RING);
    s_af_reported = last;
}

#else   // !DEVICE_RAM_REGIONS

int ram_telemetry_region_snapshot(ram_region_t *out, int max) { (void)out; (void)max; return 0; }
void ram_telemetry_regions(const char *tag) { (void)tag; }
unsigned ram_telemetry_alloc_fail_count(void) { return 0; }
void ram_telemetry_alloc_fail_flush(void) {}

#endif
