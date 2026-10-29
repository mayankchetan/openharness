#pragma once
#include <stdbool.h>
#include <stddef.h>

// The panel's strip buffer: `bytes` of RAM the display DMA can read from.
//
// With `want_psram` it takes PSRAM first. The start is aligned to the PSRAM cache line and the size rounded up to a
// whole number of lines, so the cache write-back that runs before the DMA read (esp_cache_msync in the SPI driver) covers
// this buffer and nothing else. If the line size cannot be read, or the PSRAM allocation fails, it logs a warning and
// returns internal DMA-capable RAM instead, so a full PSRAM never costs the display a buffer.
// `*in_psram` (may be NULL) says where it came from. Returns NULL only if the internal allocation fails too.
// The buffer is never freed.
void *draw_buffer_alloc(size_t bytes, bool want_psram, bool *in_psram);
