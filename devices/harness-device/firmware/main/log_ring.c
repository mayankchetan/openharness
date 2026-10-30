#include "log_ring.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// One lock for every ring (there is one). Interrupts stay off for a few loads and stores, never for a copy of a line.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define RING_LOCK()   portENTER_CRITICAL(&s_lock)
#define RING_UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING_LOCK()   pthread_mutex_lock(&s_lock)
#define RING_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

enum { ST_WRITING = 1, ST_READY = 2, ST_CLAIMED = 3 };   // the state byte of an entry (never 0 inside the ring)

void log_ring_init(log_ring_t *r, uint8_t *storage, uint32_t cap)
{
    r->buf = storage;
    r->cap = storage ? cap : 0;
    r->tail = 0;
    r->used = 0;
    atomic_init(&r->overwritten, 0);
    atomic_init(&r->rejected, 0);
}

// Byte copies with wrap. They run outside the lock, on a slot nobody else may touch.
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

// Header access, under the lock only.
static uint32_t entry_len(const log_ring_t *r, uint32_t at)
{
    return (uint32_t)r->buf[at] | ((uint32_t)r->buf[(at + 1) % r->cap] << 8);
}

static uint8_t *state_at(log_ring_t *r, uint32_t at) { return &r->buf[(at + 2) % r->cap]; }

static void count(atomic_uint *c) { atomic_fetch_add_explicit(c, 1, memory_order_relaxed); }

bool log_ring_reserve(log_ring_t *r, size_t len, log_ring_slot_t *slot)
{
    if (len == 0 || len > LOG_RING_LINE_MAX || r->cap < LOG_RING_HDR + 1 || LOG_RING_HDR + len > r->cap) {
        count(&r->rejected);
        return false;
    }
    const uint32_t need = LOG_RING_HDR + (uint32_t)len;
    bool ok = true;
    RING_LOCK();
    while (r->cap - r->used < need) {   // drop the oldest committed entries until the new one fits
        if (*state_at(r, r->tail) != ST_READY) {   // the oldest is being written or read: it cannot be recycled
            ok = false;
            break;
        }
        const uint32_t old = LOG_RING_HDR + entry_len(r, r->tail);
        r->tail = (r->tail + old) % r->cap;
        r->used -= old;
        count(&r->overwritten);
    }
    if (ok) {
        const uint32_t head = (r->tail + r->used) % r->cap;
        r->buf[head] = (uint8_t)(len & 0xFF);
        r->buf[(head + 1) % r->cap] = (uint8_t)(len >> 8);
        *state_at(r, head) = ST_WRITING;
        r->used += need;
        slot->at = head;
        slot->len = (uint32_t)len;
    }
    RING_UNLOCK();
    if (!ok) count(&r->rejected);
    return ok;
}

void log_ring_write(log_ring_t *r, const log_ring_slot_t *slot, const uint8_t *line)
{
    put(r, (slot->at + LOG_RING_HDR) % r->cap, line, slot->len);
}

void log_ring_commit(log_ring_t *r, const log_ring_slot_t *slot)
{
    RING_LOCK();
    *state_at(r, slot->at) = ST_READY;
    RING_UNLOCK();
}

bool log_ring_push(log_ring_t *r, const uint8_t *line, size_t len)
{
    if (!line || len == 0) {
        count(&r->rejected);
        return false;
    }
    if (len > LOG_RING_LINE_MAX) len = LOG_RING_LINE_MAX;
    log_ring_slot_t slot;
    if (!log_ring_reserve(r, len, &slot)) return false;
    log_ring_write(r, &slot, line);
    log_ring_commit(r, &slot);
    return true;
}

bool log_ring_claim(log_ring_t *r, size_t out_cap, log_ring_slot_t *slot)
{
    bool ok = false;
    RING_LOCK();
    if (r->used && *state_at(r, r->tail) == ST_READY) {
        const uint32_t len = entry_len(r, r->tail);
        if (len <= out_cap) {
            *state_at(r, r->tail) = ST_CLAIMED;
            slot->at = r->tail;
            slot->len = len;
            ok = true;
        }
    }
    RING_UNLOCK();
    return ok;
}

void log_ring_read(log_ring_t *r, const log_ring_slot_t *slot, uint8_t *out)
{
    get(r, (slot->at + LOG_RING_HDR) % r->cap, out, slot->len);
}

void log_ring_release(log_ring_t *r, const log_ring_slot_t *slot)
{
    RING_LOCK();
    r->tail = (slot->at + LOG_RING_HDR + slot->len) % r->cap;   // a claimed entry is the tail, and no producer may drop it
    r->used -= LOG_RING_HDR + slot->len;
    RING_UNLOCK();
}

size_t log_ring_pop(log_ring_t *r, uint8_t *out, size_t out_cap)
{
    log_ring_slot_t slot;
    if (!log_ring_claim(r, out_cap, &slot)) return 0;
    log_ring_read(r, &slot, out);
    log_ring_release(r, &slot);
    return slot.len;
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
