// Host stand-in for the one private IDF call draw_buffer.c makes (test_draw_buffer.c provides it).
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
esp_err_t esp_cache_get_alignment(uint32_t heap_caps, size_t *out_alignment);
