#pragma once
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#define MALLOC_CAP_INTERNAL (1u << 11)
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
