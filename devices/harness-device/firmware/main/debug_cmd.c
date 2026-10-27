#include "debug_cmd.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

const char *dbg_err_name(dbg_err_t err)
{
    switch (err) {
    case DBG_E_SHAPE:   return "shape";
    case DBG_E_RANGE:   return "range";
    case DBG_E_BUSY:    return "busy";
    case DBG_E_MEMORY:  return "memory";
    case DBG_E_ASLEEP:  return "asleep";
    case DBG_E_TIMEOUT: return "timeout";
    case DBG_E_TOUCH:   return "touch";
    case DBG_E_IO:      return "io";
    default:            return "error";
    }
}

// ── the screenshot ──────────────────────────────────────────────────────────────────────────────────

uint32_t dbg_crc32(uint32_t crc, const uint8_t *data, size_t length)
{
    // Sixteen entries and two steps a byte, the same trade pet_store.c makes: 64 bytes of table instead of
    // a kilobyte, on a loop that runs once per screenshot.
    static const uint32_t t[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                   0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    uint32_t c = ~crc;
    for (size_t i = 0; i < length; i++) {
        c ^= data[i];
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    return ~c;
}

uint32_t dbg_shot_chunks(uint32_t total, uint32_t chunk)
{
    if (!total || !chunk) return 0;
    return total / chunk + (total % chunk ? 1 : 0);
}

bool dbg_shot_memory_ok(size_t largest_block, size_t free_total)
{
    return largest_block >= DBG_SHOT_BYTES && free_total >= (size_t)DBG_SHOT_BYTES + DBG_SHOT_RESERVE;
}

size_t dbg_shot_chunk_encode(uint16_t id, uint32_t index, const uint8_t *pixels, uint32_t total,
                             uint32_t chunk, uint8_t *out, size_t cap)
{
    // `index` is a u16 on the wire, so a picture that needs more chunks than that cannot be described.
    if (!pixels || !out || !chunk || index > UINT16_MAX || index >= dbg_shot_chunks(total, chunk)) return 0;
    const uint32_t at = index * chunk;
    const uint32_t n = total - at < chunk ? total - at : chunk;
    if (cap < DBG_SHOT_HEADER + n) return 0;
    out[0] = (uint8_t)id;
    out[1] = (uint8_t)(id >> 8);
    out[2] = (uint8_t)index;
    out[3] = (uint8_t)(index >> 8);
    memcpy(out + DBG_SHOT_HEADER, pixels + at, n);
    return DBG_SHOT_HEADER + n;
}

// ── injected touch ──────────────────────────────────────────────────────────────────────────────────

bool dbg_point_ok(int x, int y)
{
    if (x < 0 || y < 0 || x >= HT_WIDTH || y >= HT_HEIGHT) return false;
    // Pixel centres at 2x+1 so the circle is exact in integers: the face's centre is (W, H) in those
    // units and its radius W.
    const long dx = 2L * x + 1 - HT_WIDTH, dy = 2L * y + 1 - HT_HEIGHT;
    return dx * dx + dy * dy <= (long)HT_WIDTH * HT_WIDTH;
}

// A whole number inside [lo, hi]. The cast is only reached once the range is known to hold, so it cannot
// overflow, and NAN fails both comparisons.
static dbg_err_t whole(double v, double lo, double hi, int *out)
{
    if (!isfinite(v) || v != floor(v)) return DBG_E_SHAPE;
    if (v < lo || v > hi) return DBG_E_RANGE;
    *out = (int)v;
    return DBG_OK;
}

static dbg_err_t point(double x, double y, int *ox, int *oy)
{
    // Shape errors win over range errors across both coordinates: "x is missing" is more useful than
    // "y is off the face" when both are true.
    int ix = 0, iy = 0;
    dbg_err_t ex = whole(x, INT16_MIN, INT16_MAX, &ix), ey = whole(y, INT16_MIN, INT16_MAX, &iy);
    if (ex == DBG_E_SHAPE || ey == DBG_E_SHAPE) return DBG_E_SHAPE;
    if (ex || ey || !dbg_point_ok(ix, iy)) return DBG_E_RANGE;
    *ox = ix;
    *oy = iy;
    return DBG_OK;
}

dbg_err_t dbg_tap_make(double x, double y, dbg_gesture_t *out)
{
    int ix, iy;
    dbg_err_t err = point(x, y, &ix, &iy);
    if (err) return err;
    *out = (dbg_gesture_t){DBG_TAP, (int16_t)ix, (int16_t)iy, (int16_t)ix, (int16_t)iy, DBG_TAP_MS};
    return DBG_OK;
}

dbg_err_t dbg_press_make(double x, double y, double ms, dbg_gesture_t *out)
{
    int ix, iy, held;
    dbg_err_t err = point(x, y, &ix, &iy), em = whole(ms, 0, 1e9, &held);
    if (err == DBG_E_SHAPE || em == DBG_E_SHAPE) return DBG_E_SHAPE;
    if (err || em || held < DBG_PRESS_MIN_MS || held > DBG_PRESS_MAX_MS) return DBG_E_RANGE;
    *out = (dbg_gesture_t){DBG_PRESS, (int16_t)ix, (int16_t)iy, (int16_t)ix, (int16_t)iy, (uint16_t)held};
    return DBG_OK;
}

dbg_err_t dbg_swipe_make(double x1, double y1, double x2, double y2, double ms, dbg_gesture_t *out)
{
    int ax, ay, bx, by, span;
    dbg_err_t from = point(x1, y1, &ax, &ay), to = point(x2, y2, &bx, &by), em = whole(ms, 0, 1e9, &span);
    if (from == DBG_E_SHAPE || to == DBG_E_SHAPE || em == DBG_E_SHAPE) return DBG_E_SHAPE;
    if (from || to || em || span < DBG_SWIPE_MIN_MS || span > DBG_SWIPE_MAX_MS) return DBG_E_RANGE;
    // The face is convex, so a straight line between two points on it stays on it: checking the ends is
    // checking the path.
    *out = (dbg_gesture_t){DBG_SWIPE, (int16_t)ax, (int16_t)ay, (int16_t)bx, (int16_t)by, (uint16_t)span};
    return DBG_OK;
}

dbg_sample_t dbg_gesture_at(const dbg_gesture_t *g, uint32_t elapsed_ms)
{
    const uint32_t length = g->kind == DBG_TAP ? DBG_TAP_MS : g->ms;
    dbg_sample_t s = {.down = true, .x = g->x2, .y = g->y2};
    if (elapsed_ms < length) {
        // Linear in time. Integer arithmetic with the product held in 32 bits: the deltas are under 466
        // and the length under 3000.
        s.x = (int16_t)(g->x1 + ((int32_t)(g->x2 - g->x1) * (int32_t)elapsed_ms) / (int32_t)length);
        s.y = (int16_t)(g->y1 + ((int32_t)(g->y2 - g->y1) * (int32_t)elapsed_ms) / (int32_t)length);
    } else if (elapsed_ms >= length + DBG_END_HOLD_MS) {
        s.down = false;
        s.done = true;
    }
    return s;
}

// ── the hand-off ────────────────────────────────────────────────────────────────────────────────────

enum { INJECT_IDLE, INJECT_ARMED, INJECT_RUNNING, INJECT_DONE, INJECT_FAILED };

// A gesture that the touch task has not picked up in this long never will: the task is stuck or the
// controller is down. And one that has run this much past its own length has been lost.
#define PICKUP_US INT64_C(1000000)
#define SLACK_US  INT64_C(2000000)

static atomic_int s_inject;
static dbg_gesture_t s_gesture;   // written by the arming task while IDLE, read by the touch task after ARMED
static int64_t s_armed_us, s_start_us;   // likewise: armed_us by the cable task, start_us by the touch task

bool dbg_inject_arm(const dbg_gesture_t *g, int64_t now_us)
{
    if (atomic_load_explicit(&s_inject, memory_order_acquire) != INJECT_IDLE) return false;
    s_gesture = *g;
    s_armed_us = now_us;
    atomic_store_explicit(&s_inject, INJECT_ARMED, memory_order_release);
    return true;
}

bool dbg_inject_poll(int64_t now_us, dbg_sample_t *out)
{
    // The cost of this when nobody is injecting: one load.
    int state = atomic_load_explicit(&s_inject, memory_order_acquire);
    if (state == INJECT_IDLE) return false;
    if (state == INJECT_ARMED) {
        s_start_us = now_us;
        if (!atomic_compare_exchange_strong(&s_inject, &state, INJECT_RUNNING)) return false;   // expired under us
    } else if (state != INJECT_RUNNING) {
        return false;
    }
    const int64_t since = now_us - s_start_us;
    *out = dbg_gesture_at(&s_gesture, since > 0 ? (uint32_t)(since / 1000) : 0);
    if (out->done) {
        // A late FAILED from the cable task's expiry wins: the swap only happens from RUNNING.
        state = INJECT_RUNNING;
        atomic_compare_exchange_strong(&s_inject, &state, INJECT_DONE);
    }
    return true;
}

void dbg_inject_abort(void)
{
    int state = INJECT_RUNNING;
    if (!atomic_compare_exchange_strong(&s_inject, &state, INJECT_FAILED)) {
        state = INJECT_ARMED;
        atomic_compare_exchange_strong(&s_inject, &state, INJECT_FAILED);
    }
}

int dbg_inject_finish(int64_t now_us)
{
    int state = atomic_load_explicit(&s_inject, memory_order_acquire);
    if (state == INJECT_ARMED && now_us - s_armed_us > PICKUP_US) {
        if (atomic_compare_exchange_strong(&s_inject, &state, INJECT_FAILED)) state = INJECT_FAILED;
    } else if (state == INJECT_RUNNING) {
        const int64_t length = (s_gesture.kind == DBG_TAP ? DBG_TAP_MS : s_gesture.ms) * INT64_C(1000);
        if (now_us - s_start_us > length + SLACK_US) {
            if (atomic_compare_exchange_strong(&s_inject, &state, INJECT_FAILED)) state = INJECT_FAILED;
        }
    }
    if (state != INJECT_DONE && state != INJECT_FAILED) return 0;
    atomic_store_explicit(&s_inject, INJECT_IDLE, memory_order_release);
    return state == INJECT_DONE ? 1 : -1;
}
