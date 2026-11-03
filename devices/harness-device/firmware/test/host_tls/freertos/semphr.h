// Host stand-ins for the FreeRTOS semaphores the transports use: pthreads underneath, one tick = one millisecond.
//   xSemaphoreCreateMutex()  a non-recursive mutex (taking it twice in one thread deadlocks, as a bug should).
//   xSemaphoreCreateBinary() a binary semaphore: Give sets it (saturating), Take clears it and may wait; given and taken by different threads.
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "FreeRTOS.h"

typedef struct host_mutex {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             binary;
    int             count;   // binary only: 0 or 1
} *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    if (s) pthread_mutex_init(&s->m, NULL);
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->binary = 1;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    return s;
}

static inline void host_sem_deadline(struct timespec *ts, TickType_t ticks)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ticks / 1000;
    ts->tv_nsec += (long)(ticks % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (s->binary) {
        struct timespec ts;
        if (ticks != portMAX_DELAY && ticks != 0) host_sem_deadline(&ts, ticks);
        pthread_mutex_lock(&s->m);
        while (s->count == 0) {
            if (ticks == 0) { pthread_mutex_unlock(&s->m); return pdFALSE; }
            const int rc = ticks == portMAX_DELAY ? pthread_cond_wait(&s->c, &s->m) : pthread_cond_timedwait(&s->c, &s->m, &ts);
            if (rc == ETIMEDOUT && s->count == 0) { pthread_mutex_unlock(&s->m); return pdFALSE; }
        }
        s->count = 0;
        pthread_mutex_unlock(&s->m);
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) return pthread_mutex_lock(&s->m) == 0 ? pdTRUE : pdFALSE;
    if (ticks == 0) return pthread_mutex_trylock(&s->m) == 0 ? pdTRUE : pdFALSE;
    struct timespec ts;
    host_sem_deadline(&ts, ticks);
    return pthread_mutex_timedlock(&s->m, &ts) == 0 ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s->binary) {
        pthread_mutex_lock(&s->m);
        s->count = 1;
        pthread_cond_signal(&s->c);
        pthread_mutex_unlock(&s->m);
        return pdTRUE;
    }
    return pthread_mutex_unlock(&s->m) == 0 ? pdTRUE : pdFALSE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    pthread_mutex_destroy(&s->m);
    if (s->binary) pthread_cond_destroy(&s->c);
    free(s);
}
