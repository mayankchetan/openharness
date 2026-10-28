#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Install the global PSRAM-first cJSON allocator and initialize RAM counters.
// Call once at the very start of app_main(), before any cJSON use.
void ram_telemetry_init(void);

// Mark LVGL ready for safe lv_mem_monitor() snapshots. The display mutex must
// already exist and lv_init() plus all custom pools must have completed.

// Log internal 8-bit heap, PSRAM, LVGL, cJSON fallback, and current-task stack
// high-water data. Safe before LVGL is initialized.
void ram_telemetry_checkpoint(const char *tag);

// Periodic checkpoint. Every tenth call also records the known long-lived task
// stack high-water marks.
void ram_telemetry_periodic(const char *tag);

// PSRAM-only application allocations. They never silently consume internal RAM
// and record failures in the telemetry counters.
void *ram_psram_alloc(size_t bytes, const char *owner);
void *ram_psram_calloc(size_t count, size_t size, const char *owner);
void *ram_psram_realloc(void *ptr, size_t bytes, const char *owner);
char *ram_psram_strdup(const char *src, const char *owner);

// ---- diet-2 telemetry (RAM_REGIONS, HWM, failed allocations, flash mmap) --------------------------------------
// The ten application tasks whose stack high-water marks are reported. The names are the strings the
// xTaskCreate* calls in main/ pass, so a rename there must be repeated in ram_telemetry.c (k_hwm_tasks).
//
// One line: "HWM up=<s>s <task>=<free_min bytes> ..." (-1 = that task is not running). Cheap; no heap walk.
void ram_telemetry_hwm(void);

// True while a persistent flash mmap exists. The flash driver keeps the cache on for a flash write only when
// this is false and the app runs from PSRAM (spi_flash_os_func_app.c:239). Takes the driver's mmap freeze lock
// (flash_mmap_lock_freeze(), which waits with portMAX_DELAY, spi_flash/flash_mmap.c:148-165) and so can block
// for as long as another task holds a mapping or writes flash. NEVER call it from the render task or any
// task that must stay live; the alive line prints the cached value from ram_flash_mmap_remain_cached().
bool ram_flash_mmap_remain(void);

// Last value sampled by ram_telemetry_background_tick(): 0 or 1, or -1 before the first sample. Lock-free.
int ram_flash_mmap_remain_cached(void);

// Call from ONE background task about once a second (the refresh task's loop in app_main.c). It
//  - samples ram_flash_mmap_remain() into the cache every 10 s (this call may wait for a flash write);
//  - logs RAM_REGIONS tag=t5min first 61 s after boot, then every 300 s;
//  - flushes the failed-allocation ring.
// It is cheap when nothing is due. Do not call it from the render task.
void ram_telemetry_background_tick(void);

// Internal heap, one record per registered heap region (the five/eight regions of the boot log).
typedef struct {
    uintptr_t start;
    uint32_t size;       // end - start
    uint32_t free_b;     // free bytes now
    uint32_t largest;    // largest free block now
    uint32_t min_free;   // lowest free bytes since boot, for this region only
} ram_region_t;

// Fill `out` (up to `max` entries) and return the number of internal regions (may exceed `max`).
// Built only with DEVICE_RAM_REGIONS (CMake option, default ON on fw-ram-diet-2); returns 0 without it.
int ram_telemetry_region_snapshot(ram_region_t *out, int max);

// Log one "RAM_REGIONS tag=.. idx=i/n start=.. size=.. free=.. largest=.. min_free=.." line per region.
// Boot tags and ram_telemetry_background_tick() (61 s, then every 300 s) call it. Never call it from the 20 ms render loop.
void ram_telemetry_regions(const char *tag);

// Failed heap allocations. The IDF callback only counts and records (it can run in any context); the log
// lines come from ram_telemetry_alloc_fail_flush(), at most 4 lines per call and one flush per second.
unsigned ram_telemetry_alloc_fail_count(void);
void ram_telemetry_alloc_fail_flush(void);
