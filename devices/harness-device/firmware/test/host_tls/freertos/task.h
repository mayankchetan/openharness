#pragma once
#include <unistd.h>
#include "FreeRTOS.h"
static inline void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000); }
