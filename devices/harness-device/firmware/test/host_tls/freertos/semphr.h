// Host mutex with the FreeRTOS call shapes (non-recursive: taking it twice in one thread deadlocks, as a bug should).
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "FreeRTOS.h"

typedef struct host_mutex { pthread_mutex_t m; } *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t s = malloc(sizeof(*s));
    if (s) pthread_mutex_init(&s->m, NULL);
    return s;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (ticks == portMAX_DELAY) return pthread_mutex_lock(&s->m) == 0 ? pdTRUE : pdFALSE;
    if (ticks == 0) return pthread_mutex_trylock(&s->m) == 0 ? pdTRUE : pdFALSE;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ticks / 1000;
    ts.tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_mutex_timedlock(&s->m, &ts) == 0 ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return pthread_mutex_unlock(&s->m) == 0 ? pdTRUE : pdFALSE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { pthread_mutex_destroy(&s->m); free(s); }
