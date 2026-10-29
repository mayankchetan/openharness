// draw_buffer_alloc(): the panel's strip buffer, in PSRAM when asked and possible, else internal DMA RAM.
// The IDF calls are fakes; the test proves which one runs, with which arguments, and what comes back.
#include "draw_buffer.h"
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"

static size_t cache_line;                 // what esp_cache_get_alignment reports (0 = no answer)
static int cache_rc, cache_calls, aligned_calls, malloc_calls, warnings;
static alignas(128) unsigned char psram_pool[1 << 16], internal_pool[1 << 16];
static int psram_ok, internal_ok;         // 1 = the fake allocator succeeds
static struct { size_t align, size; uint32_t caps; } last_aligned;
static struct { size_t size; uint32_t caps; } last_malloc;

esp_err_t esp_cache_get_alignment(uint32_t caps, size_t *out)
{
    cache_calls++;
    assert(caps == MALLOC_CAP_SPIRAM);
    if (cache_rc == ESP_OK) *out = cache_line;
    return cache_rc;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    aligned_calls++;
    last_aligned.align = alignment; last_aligned.size = size; last_aligned.caps = caps;
    assert(size <= sizeof psram_pool);
    return psram_ok ? psram_pool + alignment : NULL;   // one alignment step in: aligned, and never the pool start
}
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    malloc_calls++;
    last_malloc.size = size; last_malloc.caps = caps;
    return internal_ok ? internal_pool + 3 : NULL;
}
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; warnings++; }

static void reset(size_t line, int rc, int psram, int internal)
{
    cache_line = line; cache_rc = rc; psram_ok = psram; internal_ok = internal;
    cache_calls = aligned_calls = malloc_calls = warnings = 0;
    memset(&last_aligned, 0, sizeof last_aligned); memset(&last_malloc, 0, sizeof last_malloc);
}

int main(void)
{
    bool in_psram;
    void *p;

    // PSRAM wanted and available: cache-line aligned start, size rounded up to the line, 8-bit capable PSRAM.
    // The dial's strip is 466 x 24 x 2 = 22,368 B = 699 x 32: already a whole number of 32 B lines.
    reset(32, ESP_OK, 1, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p == psram_pool + 32 && in_psram);
    assert(cache_calls == 1 && aligned_calls == 1 && malloc_calls == 0 && warnings == 0);
    assert(last_aligned.align == 32 && last_aligned.size == 22368);
    assert(last_aligned.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(((uintptr_t)p % 32) == 0);

    // A size that is not a whole number of lines is rounded up (the cache write-back before the DMA then covers only
    // this buffer), whatever the line size is.
    reset(64, ESP_OK, 1, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p && in_psram && last_aligned.align == 64 && last_aligned.size == 22400);
    reset(16, ESP_OK, 1, 1);
    p = draw_buffer_alloc(100, true, &in_psram);
    assert(p && in_psram && last_aligned.align == 16 && last_aligned.size == 112);

    // PSRAM allocation fails: the same buffer from internal DMA RAM, one warning, not in PSRAM.
    reset(32, ESP_OK, 0, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p == internal_pool + 3 && !in_psram && aligned_calls == 1 && malloc_calls == 1 && warnings == 1);
    assert(last_malloc.size == 22368 && last_malloc.caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    // The cache line size cannot be read, or is zero, or is not a power of two: no PSRAM attempt at all.
    reset(32, ESP_FAIL, 1, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p == internal_pool + 3 && !in_psram && aligned_calls == 0 && malloc_calls == 1 && warnings == 1);
    reset(0, ESP_OK, 1, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p == internal_pool + 3 && !in_psram && aligned_calls == 0);
    reset(48, ESP_OK, 1, 1);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(p == internal_pool + 3 && !in_psram && aligned_calls == 0);

    // PSRAM not wanted (the first buffer, or the switch off): internal DMA RAM, and PSRAM is never asked about.
    reset(32, ESP_OK, 1, 1);
    p = draw_buffer_alloc(22368, false, &in_psram);
    assert(p == internal_pool + 3 && !in_psram && cache_calls == 0 && aligned_calls == 0 && warnings == 0);
    assert(last_malloc.size == 22368 && last_malloc.caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    // Nothing left anywhere: NULL (the caller asserts), and the flag says not PSRAM. in_psram may be NULL.
    reset(32, ESP_OK, 0, 0);
    p = draw_buffer_alloc(22368, true, &in_psram);
    assert(!p && !in_psram);
    reset(32, ESP_OK, 1, 1);
    assert(draw_buffer_alloc(22368, true, NULL));

    puts("draw buffer: PSRAM line-aligned and rounded, internal DMA fallback on every failure, no PSRAM unless asked PASS");
    return 0;
}
