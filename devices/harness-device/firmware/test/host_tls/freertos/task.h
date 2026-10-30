#pragma once
#include <unistd.h>
#include "FreeRTOS.h"
// A test may install a hook that runs at the start of every vTaskDelay (to park a thread at a known point).
extern void (*host_tls_delay_hook)(void);
static inline void vTaskDelay(TickType_t ticks)
{
    if (host_tls_delay_hook) host_tls_delay_hook();
    usleep((useconds_t)ticks * 1000);
}
