#pragma once
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL (1u << 11)
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM (1u << 10)
#endif
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_8BIT (1u << 2)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
