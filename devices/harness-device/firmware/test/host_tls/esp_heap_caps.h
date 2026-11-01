// Host stand-in for esp_heap_caps.h: every capability is plain calloc / malloc.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
static inline void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
static inline void heap_caps_free(void *p) { free(p); }
