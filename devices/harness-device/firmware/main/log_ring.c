#include "log_ring.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// One lock for every ring (there is one). Interrupts stay off for the length of one copy, so a task cannot be preempted inside it by a
// higher-priority task that also logs, and the other core's wait is as short.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define RING_LOCK()   portENTER_CRITICAL(&s_lock)
#define RING_UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING_LOCK()   pthread_mutex_lock(&s_lock)
#define RING_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

#define HDR 2u   // the 16-bit length in front of each line

void log_ring_init(log_ring_t *r, uint8_t *storage, uint32_t cap)
{
    r->buf = storage;
    r->cap = storage ? cap : 0;
    r->tail = 0;
    r->used = 0;
    atomic_init(&r->overwritten, 0);
    atomic_init(&r->rejected, 0);
}

// The helpers below run under the lock. Offsets wrap at cap; a copy that crosses the end is two memcpy calls.
static void put(log_ring_t *r, uint32_t at, const uint8_t *src, uint32_t n)
{
    const uint32_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(r->buf + at, src, first);
    if (n > first) memcpy(r->buf, src + first, n - first);
}

static void get(const log_ring_t *r, uint32_t at, uint8_t *dst, uint32_t n)
{
    const uint32_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(dst, r->buf + at, first);
    if (n > first) memcpy(dst + first, r->buf, n - first);
}

static uint32_t entry_len(const log_ring_t *r, uint32_t at)
{
    uint8_t h[HDR];
    get(r, at, h, HDR);
    return (uint32_t)h[0] | ((uint32_t)h[1] << 8);
}

bool log_ring_push(log_ring_t *r, const uint8_t *line, size_t len)
{
    if (!line || len == 0 || r->cap < HDR + 1) {
        atomic_fetch_add_explicit(&r->rejected, 1, memory_order_relaxed);
        return false;
    }
    if (len > LOG_RING_LINE_MAX) len = LOG_RING_LINE_MAX;
    const uint32_t need = HDR + (uint32_t)len;
    if (need > r->cap) {
        atomic_fetch_add_explicit(&r->rejected, 1, memory_order_relaxed);
        return false;
    }
    const uint8_t hdr[HDR] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    RING_LOCK();
    while (r->cap - r->used < need) {   // drop the oldest until the new line fits
        const uint32_t old = HDR + entry_len(r, r->tail);
        r->tail = (r->tail + old) % r->cap;
        r->used -= old;
        atomic_fetch_add_explicit(&r->overwritten, 1, memory_order_relaxed);
    }
    const uint32_t head = (r->tail + r->used) % r->cap;
    put(r, head, hdr, HDR);
    put(r, (head + HDR) % r->cap, line, (uint32_t)len);
    r->used += need;
    RING_UNLOCK();
    return true;
}

size_t log_ring_pop(log_ring_t *r, uint8_t *out, size_t out_cap)
{
    size_t n = 0;
    RING_LOCK();
    if (r->used) {
        const uint32_t len = entry_len(r, r->tail);
        if (len <= out_cap) {
            get(r, (r->tail + HDR) % r->cap, out, len);
            r->tail = (r->tail + HDR + len) % r->cap;
            r->used -= HDR + len;
            n = len;
        }
    }
    RING_UNLOCK();
    return n;
}

uint32_t log_ring_used(log_ring_t *r)
{
    RING_LOCK();
    const uint32_t used = r->used;
    RING_UNLOCK();
    return used;
}

uint32_t log_ring_overwritten(const log_ring_t *r) { return atomic_load_explicit(&r->overwritten, memory_order_relaxed); }
uint32_t log_ring_rejected(const log_ring_t *r) { return atomic_load_explicit(&r->rejected, memory_order_relaxed); }
