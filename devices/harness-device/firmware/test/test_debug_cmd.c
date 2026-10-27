// Host test for debug_cmd.c: the screenshot's chunking and checksum, the face geometry, the gesture
// sampler and the hand-off between the cable task and the touch task. No ESP-IDF, no cJSON — the JSON half
// is test_debug_cmd_json.c.
#include "debug_cmd.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── crc32 ───────────────────────────────────────────────────────────────────────────────────────────
// The reference is the textbook bit-at-a-time form, so the nibble table in the firmware is judged against
// something that does not share its table.
static uint32_t crc32_bitwise(uint32_t crc, const uint8_t *d, size_t n)
{
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void test_crc32(void)
{
    // The published check value for CRC-32/ISO-HDLC, and what Python's zlib.crc32 returns for it.
    assert(dbg_crc32(0, (const uint8_t *)"123456789", 9) == 0xcbf43926u);
    assert(dbg_crc32(0, NULL, 0) == 0);
    uint8_t data[5000];
    unsigned seed = 7;
    for (size_t i = 0; i < sizeof data; i++) { seed = seed * 1103515245u + 12345u; data[i] = (uint8_t)(seed >> 16); }
    assert(dbg_crc32(0, data, sizeof data) == crc32_bitwise(0, data, sizeof data));
    // Running form: any split gives the same answer as one pass — which is how the sender computes it.
    for (size_t cut = 0; cut <= sizeof data; cut += 333) {
        uint32_t c = dbg_crc32(0, data, cut);
        assert(dbg_crc32(c, data + cut, sizeof data - cut) == dbg_crc32(0, data, sizeof data));
    }
    // A single flipped bit is always seen.
    uint32_t good = dbg_crc32(0, data, sizeof data);
    for (size_t i = 0; i < sizeof data; i += 97) {
        data[i] ^= 0x10;
        assert(dbg_crc32(0, data, sizeof data) != good);
        data[i] ^= 0x10;
    }
}

// ── chunking ────────────────────────────────────────────────────────────────────────────────────────

static void test_chunking(void)
{
    assert(DBG_SHOT_BYTES == 434312u && DBG_SHOT_BYTES == 466u * 466u * 2u);
    assert(dbg_shot_chunks(DBG_SHOT_BYTES, DBG_SHOT_CHUNK) == 425);
    assert(dbg_shot_chunks(1024, 1024) == 1 && dbg_shot_chunks(1025, 1024) == 2 && dbg_shot_chunks(1, 1024) == 1);
    assert(dbg_shot_chunks(0, 1024) == 0 && dbg_shot_chunks(1024, 0) == 0);
    // The chunk index is a u16 on the wire; the picture must fit in it with room to spare.
    assert(dbg_shot_chunks(DBG_SHOT_BYTES, DBG_SHOT_CHUNK) < UINT16_MAX);
    // And one chunk must fit a frame: header plus pixels under the protocol's payload ceiling (8192).
    assert(DBG_SHOT_HEADER + DBG_SHOT_CHUNK <= 8192);

    uint8_t *picture = malloc(DBG_SHOT_BYTES), *rebuilt = malloc(DBG_SHOT_BYTES);
    assert(picture && rebuilt);
    unsigned seed = 1;
    for (uint32_t i = 0; i < DBG_SHOT_BYTES; i++) { seed = seed * 1664525u + 1013904223u; picture[i] = (uint8_t)(seed >> 24); }
    memset(rebuilt, 0, DBG_SHOT_BYTES);

    const uint32_t chunks = dbg_shot_chunks(DBG_SHOT_BYTES, DBG_SHOT_CHUNK);
    uint8_t out[DBG_SHOT_HEADER + DBG_SHOT_CHUNK];
    uint32_t crc = 0, total = 0;
    for (uint32_t i = 0; i < chunks; i++) {
        size_t n = dbg_shot_chunk_encode(0xbeef, i, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, sizeof out);
        assert(n > DBG_SHOT_HEADER);
        // Little-endian id then index, whatever the host's byte order.
        assert(out[0] == 0xef && out[1] == 0xbe && out[2] == (i & 0xff) && out[3] == (i >> 8));
        // Every chunk is full except the last, which is the remainder: 136 bytes.
        assert(n - DBG_SHOT_HEADER == (i == chunks - 1 ? 136u : 1024u));
        memcpy(rebuilt + i * DBG_SHOT_CHUNK, out + DBG_SHOT_HEADER, n - DBG_SHOT_HEADER);
        crc = dbg_crc32(crc, out + DBG_SHOT_HEADER, n - DBG_SHOT_HEADER);
        total += (uint32_t)(n - DBG_SHOT_HEADER);
    }
    assert(total == DBG_SHOT_BYTES && !memcmp(picture, rebuilt, DBG_SHOT_BYTES));
    assert(crc == dbg_crc32(0, picture, DBG_SHOT_BYTES) && crc == crc32_bitwise(0, picture, DBG_SHOT_BYTES));

    // Loss is visible three ways: a missing index, a short total, and a different checksum.
    uint32_t without = 0;
    for (uint32_t i = 0; i < chunks; i++) {
        if (i == 200) continue;
        size_t n = dbg_shot_chunk_encode(1, i, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, sizeof out);
        without = dbg_crc32(without, out + DBG_SHOT_HEADER, n - DBG_SHOT_HEADER);
    }
    assert(without != crc);

    // Refusals: past the end, buffer too small, nothing to read, nothing to write, a one-byte-short cap.
    assert(!dbg_shot_chunk_encode(1, chunks, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, sizeof out));
    assert(!dbg_shot_chunk_encode(1, 0, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, DBG_SHOT_HEADER + DBG_SHOT_CHUNK - 1));
    assert(dbg_shot_chunk_encode(1, 0, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, DBG_SHOT_HEADER + DBG_SHOT_CHUNK));
    assert(!dbg_shot_chunk_encode(1, chunks - 1, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, DBG_SHOT_HEADER + 135));
    assert(dbg_shot_chunk_encode(1, chunks - 1, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, DBG_SHOT_HEADER + 136));
    assert(!dbg_shot_chunk_encode(1, 0, NULL, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, out, sizeof out));
    assert(!dbg_shot_chunk_encode(1, 0, picture, DBG_SHOT_BYTES, DBG_SHOT_CHUNK, NULL, sizeof out));
    assert(!dbg_shot_chunk_encode(1, 0, picture, DBG_SHOT_BYTES, 0, out, sizeof out));
    // An index that does not fit the u16 is refused even when the buffer would be big enough.
    assert(!dbg_shot_chunk_encode(1, 70000, picture, 70001u * 4, 4, out, sizeof out));
    assert(dbg_shot_chunk_encode(1, 65535, picture, 65536u * 4, 4, out, sizeof out) == DBG_SHOT_HEADER + 4);
    free(picture);
    free(rebuilt);
}

static void test_memory_policy(void)
{
    const size_t need = DBG_SHOT_BYTES, keep = DBG_SHOT_RESERVE;
    assert(dbg_shot_memory_ok(need, need + keep));
    assert(!dbg_shot_memory_ok(need - 1, need + keep));      // total is plenty, no block is big enough
    assert(!dbg_shot_memory_ok(need, need + keep - 1));      // a block fits but would leave under the reserve
    assert(dbg_shot_memory_ok(4500u * 1024, 4500u * 1024));  // the 4.5 MB the board has free
    assert(!dbg_shot_memory_ok(0, 0));
    assert(!dbg_shot_memory_ok(200u * 1024, 4500u * 1024));  // fragmented
}

// ── the face ────────────────────────────────────────────────────────────────────────────────────────

static void test_face(void)
{
    assert(dbg_point_ok(233, 233) && dbg_point_ok(232, 232));
    // The four extremes of the disc are on it, one pixel beyond is not.
    assert(dbg_point_ok(0, 232) && dbg_point_ok(465, 233) && dbg_point_ok(232, 0) && dbg_point_ok(233, 465));
    assert(!dbg_point_ok(-1, 233) && !dbg_point_ok(466, 233) && !dbg_point_ok(233, -1) && !dbg_point_ok(233, 466));
    // The corners of the square buffer are glass-free.
    assert(!dbg_point_ok(0, 0) && !dbg_point_ok(465, 0) && !dbg_point_ok(0, 465) && !dbg_point_ok(465, 465));
    assert(!dbg_point_ok(50, 50) && dbg_point_ok(70, 70));
    // The area of the disc, counted: pi r^2 for r = 233 is 170,573; the pixel count must be that to a hair.
    unsigned inside = 0;
    for (int y = 0; y < HT_HEIGHT; y++)
        for (int x = 0; x < HT_WIDTH; x++) {
            bool ok = dbg_point_ok(x, y);
            inside += ok;
            // Symmetric about both axes: no off-by-one favouring a side.
            assert(ok == dbg_point_ok(HT_WIDTH - 1 - x, y) && ok == dbg_point_ok(x, HT_HEIGHT - 1 - y));
        }
    assert(inside > 170000 && inside < 171200);
    assert(!dbg_point_ok(INT32_MAX, 0) && !dbg_point_ok(0, INT32_MIN));
}

static void test_make(void)
{
    dbg_gesture_t g;
    assert(dbg_tap_make(233, 220, &g) == DBG_OK && g.kind == DBG_TAP && g.x1 == 233 && g.y1 == 220 &&
           g.x2 == 233 && g.y2 == 220 && g.ms == DBG_TAP_MS);
    assert(dbg_tap_make(0, 232, &g) == DBG_OK);
    assert(dbg_tap_make(0, 0, &g) == DBG_E_RANGE);                 // off the round glass
    assert(dbg_tap_make(466, 233, &g) == DBG_E_RANGE);
    assert(dbg_tap_make(-1, 233, &g) == DBG_E_RANGE);
    assert(dbg_tap_make(1e300, 233, &g) == DBG_E_RANGE);           // never reaches an int cast
    assert(dbg_tap_make(-1e300, 233, &g) == DBG_E_RANGE);
    assert(dbg_tap_make(233.5, 233, &g) == DBG_E_SHAPE);           // a typo, not a request to round
    assert(dbg_tap_make(NAN, 233, &g) == DBG_E_SHAPE);             // a missing field
    assert(dbg_tap_make(233, NAN, &g) == DBG_E_SHAPE);
    assert(dbg_tap_make(INFINITY, 233, &g) == DBG_E_SHAPE);
    assert(dbg_tap_make(NAN, 0, &g) == DBG_E_SHAPE);               // shape outranks range across the pair
    assert(dbg_tap_make(0, NAN, &g) == DBG_E_SHAPE);

    assert(dbg_press_make(233, 233, 700, &g) == DBG_OK && g.kind == DBG_PRESS && g.ms == 700);
    assert(dbg_press_make(233, 233, DBG_PRESS_MIN_MS, &g) == DBG_OK);
    assert(dbg_press_make(233, 233, DBG_PRESS_MAX_MS, &g) == DBG_OK);
    assert(dbg_press_make(233, 233, DBG_PRESS_MIN_MS - 1, &g) == DBG_E_RANGE);
    assert(dbg_press_make(233, 233, DBG_PRESS_MAX_MS + 1, &g) == DBG_E_RANGE);
    assert(dbg_press_make(233, 233, -5, &g) == DBG_E_RANGE);
    assert(dbg_press_make(233, 233, 1e300, &g) == DBG_E_RANGE);
    assert(dbg_press_make(233, 233, NAN, &g) == DBG_E_SHAPE);
    assert(dbg_press_make(233, 233, 99.5, &g) == DBG_E_SHAPE);
    assert(dbg_press_make(0, 0, 700, &g) == DBG_E_RANGE);
    assert(dbg_press_make(0, 0, NAN, &g) == DBG_E_SHAPE);

    assert(dbg_swipe_make(100, 233, 300, 233, 200, &g) == DBG_OK && g.kind == DBG_SWIPE && g.x1 == 100 && g.x2 == 300 &&
           g.y1 == 233 && g.y2 == 233 && g.ms == 200);
    assert(dbg_swipe_make(233, 233, 233, 233, DBG_SWIPE_MIN_MS, &g) == DBG_OK);
    assert(dbg_swipe_make(233, 233, 233, 233, DBG_SWIPE_MAX_MS, &g) == DBG_OK);
    assert(dbg_swipe_make(233, 233, 233, 233, DBG_SWIPE_MIN_MS - 1, &g) == DBG_E_RANGE);
    assert(dbg_swipe_make(233, 233, 233, 233, DBG_SWIPE_MAX_MS + 1, &g) == DBG_E_RANGE);
    assert(dbg_swipe_make(0, 0, 233, 233, 200, &g) == DBG_E_RANGE);     // either end off the face
    assert(dbg_swipe_make(233, 233, 465, 465, 200, &g) == DBG_E_RANGE);
    assert(dbg_swipe_make(233, 233, 300, NAN, 200, &g) == DBG_E_SHAPE);
    assert(dbg_swipe_make(233, 233, 300, 300, NAN, &g) == DBG_E_SHAPE);
    assert(dbg_swipe_make(NAN, 233, 0, 0, 200, &g) == DBG_E_SHAPE);
}

// ── the sampler ─────────────────────────────────────────────────────────────────────────────────────

static void test_sampler(void)
{
    dbg_gesture_t g;
    assert(dbg_tap_make(120, 300, &g) == DBG_OK);
    for (uint32_t t = 0; t < DBG_TAP_MS + DBG_END_HOLD_MS; t++) {
        dbg_sample_t s = dbg_gesture_at(&g, t);
        assert(s.down && !s.done && s.x == 120 && s.y == 300);
    }
    dbg_sample_t up = dbg_gesture_at(&g, DBG_TAP_MS + DBG_END_HOLD_MS);
    assert(!up.down && up.done && up.x == 120 && up.y == 300);
    assert(dbg_gesture_at(&g, 100000).done);

    assert(dbg_press_make(233, 233, 700, &g) == DBG_OK);
    assert(dbg_gesture_at(&g, 699).down && dbg_gesture_at(&g, 700 + DBG_END_HOLD_MS - 1).down);
    assert(dbg_gesture_at(&g, 700 + DBG_END_HOLD_MS).done);

    // Left to right across the middle: starts on the start, ends on the end, goes through the middle at the
    // middle, and always moves forward.
    assert(dbg_swipe_make(100, 233, 300, 233, 200, &g) == DBG_OK);
    assert(dbg_gesture_at(&g, 0).x == 100 && dbg_gesture_at(&g, 100).x == 200 && dbg_gesture_at(&g, 199).x == 299);
    dbg_sample_t end = dbg_gesture_at(&g, 200);
    assert(end.down && !end.done && end.x == 300 && end.y == 233);    // the end is delivered as a contact
    assert(dbg_gesture_at(&g, 200 + DBG_END_HOLD_MS - 1).down);
    up = dbg_gesture_at(&g, 200 + DBG_END_HOLD_MS);
    assert(!up.down && up.done && up.x == 300 && up.y == 233);
    int last = -1;
    for (uint32_t t = 0; t < 200; t++) { int x = dbg_gesture_at(&g, t).x; assert(x >= last); last = x; }

    // Backwards and diagonal: the same arithmetic, the other signs.
    assert(dbg_swipe_make(300, 100, 100, 300, 400, &g) == DBG_OK);
    dbg_sample_t mid = dbg_gesture_at(&g, 200);
    assert(mid.x == 200 && mid.y == 200);
    assert(dbg_gesture_at(&g, 0).x == 300 && dbg_gesture_at(&g, 0).y == 100);

    // The path never leaves the glass for any pair of valid ends, at any time.
    unsigned seed = 3;
    for (int n = 0; n < 4000; n++) {
        int v[4];
        for (int k = 0; k < 4; k++) { seed = seed * 1103515245u + 12345u; v[k] = (int)((seed >> 8) % 466); }
        if (dbg_swipe_make(v[0], v[1], v[2], v[3], 20 + n % 2000, &g) != DBG_OK) continue;
        for (uint32_t t = 0; t < g.ms; t += 7) {
            dbg_sample_t s = dbg_gesture_at(&g, t);
            assert(s.down && dbg_point_ok(s.x, s.y));
        }
    }
}

// ── the hand-off ────────────────────────────────────────────────────────────────────────────────────

static void test_inject(void)
{
    dbg_gesture_t g;
    dbg_sample_t s;
    assert(dbg_tap_make(233, 233, &g) == DBG_OK);
    const int64_t ms = 1000;

    // Idle costs nothing and says nothing.
    assert(!dbg_inject_poll(5 * ms, &s) && dbg_inject_finish(5 * ms) == 0);

    // Arm, and a second arm is refused until the first is collected.
    assert(dbg_inject_arm(&g, 10 * ms));
    assert(!dbg_inject_arm(&g, 10 * ms));
    assert(dbg_inject_finish(11 * ms) == 0);          // armed, not yet picked up, not yet late

    // The touch task's passes: down for the tap, then the closing sample once.
    int64_t t0 = 20 * ms;
    assert(dbg_inject_poll(t0, &s) && s.down && s.x == 233);
    assert(dbg_inject_poll(t0 + 30 * ms, &s) && s.down);
    assert(dbg_inject_finish(t0 + 30 * ms) == 0);     // not finished until the finger lifts
    assert(dbg_inject_poll(t0 + 80 * ms, &s) && !s.down && s.done);
    assert(!dbg_inject_poll(t0 + 90 * ms, &s));       // the closing sample is delivered exactly once
    assert(!dbg_inject_arm(&g, t0 + 90 * ms));        // and the slot stays taken until it is collected
    assert(dbg_inject_finish(t0 + 90 * ms) == 1);
    assert(dbg_inject_finish(t0 + 91 * ms) == 0);     // collected once
    assert(!dbg_inject_poll(t0 + 100 * ms, &s));

    // The task never picks it up: reported as a failure after a second, and the slot comes back.
    assert(dbg_inject_arm(&g, 100 * ms));
    assert(dbg_inject_finish(100 * ms + 900 * 1000) == 0);
    assert(dbg_inject_finish(100 * ms + 1100 * 1000) == -1);
    assert(!dbg_inject_poll(100 * ms + 1200 * 1000, &s));   // a failed gesture is not played late
    assert(dbg_inject_arm(&g, 200 * ms));
    assert(dbg_inject_poll(200 * ms, &s));
    assert(dbg_inject_poll(200 * ms + 80 * ms, &s) && s.done);
    assert(dbg_inject_finish(0) == 1);

    // Lost partway (the touch task stalled): given up on after its length plus the slack.
    assert(dbg_swipe_make(100, 233, 300, 233, 200, &g) == DBG_OK);
    assert(dbg_inject_arm(&g, 300 * ms));
    assert(dbg_inject_poll(300 * ms, &s) && s.down && s.x == 100);
    assert(dbg_inject_finish(300 * ms + 2000 * 1000) == 0);
    assert(dbg_inject_finish(300 * ms + 2300 * 1000) == -1);
    assert(!dbg_inject_poll(300 * ms + 2400 * 1000, &s));

    // The controller goes away: the task aborts what it holds, whether armed or running.
    assert(dbg_inject_arm(&g, 400 * ms));
    dbg_inject_abort();
    assert(dbg_inject_finish(400 * ms) == -1);
    assert(dbg_inject_arm(&g, 410 * ms));
    assert(dbg_inject_poll(410 * ms, &s));
    dbg_inject_abort();
    assert(!dbg_inject_poll(420 * ms, &s) && dbg_inject_finish(420 * ms) == -1);
    dbg_inject_abort();                               // aborting with nothing in flight is harmless
    assert(dbg_inject_finish(430 * ms) == 0);

    // A clock that stepped backwards between arm and first poll never yields a huge elapsed time.
    assert(dbg_inject_arm(&g, 500 * ms));
    assert(dbg_inject_poll(400 * ms, &s) && s.down && s.x == 100);
    assert(dbg_inject_poll(400 * ms - 5 * ms, &s) && s.down && s.x == 100);
    while (!dbg_inject_poll(400 * ms + 500 * ms, &s) || !s.done) {}
    assert(dbg_inject_finish(0) == 1);
}

// One cable task arming and collecting, one touch task playing, on real threads. The assertion is that
// every gesture gets exactly one outcome and the touch side never sees a closing sample twice.
static volatile int s_stop;
static unsigned s_closing_seen;
static void *touch_thread(void *arg)
{
    (void)arg;
    int64_t now = 1000000;
    while (!s_stop) {
        dbg_sample_t s;
        if (dbg_inject_poll(now, &s) && s.done) s_closing_seen++;
        now += 4000;   // the 4 ms pass of the real task
    }
    return NULL;
}

static void test_inject_threads(void)
{
    dbg_gesture_t g;
    assert(dbg_swipe_make(100, 233, 300, 233, 20, &g) == DBG_OK);
    pthread_t thread;
    s_stop = 0;
    s_closing_seen = 0;
    assert(!pthread_create(&thread, NULL, touch_thread, NULL));
    unsigned done = 0, failed = 0;
    for (int n = 0; n < 300; n++) {
        while (!dbg_inject_arm(&g, 1000000)) {}
        // Collect: with the clock the touch thread runs on far ahead of this arm time, a gesture either
        // plays out or (rarely) is given up on; both count as one outcome.
        int r;
        int64_t clock = 1000000;
        while (!(r = dbg_inject_finish(clock))) clock += 1000;
        if (r > 0) done++; else failed++;
    }
    s_stop = 1;
    pthread_join(thread, NULL);
    assert(done + failed == 300);
    assert(done > 0);
    // A gesture the cable task gives up on after its closing sample was already played still counts as a
    // failure to the host, so the touch side may have seen more closings than there were successes.
    assert(s_closing_seen >= done && s_closing_seen <= 300);
    assert(dbg_inject_finish(0) == 0);
}

// ── names ───────────────────────────────────────────────────────────────────────────────────────────

static void test_names(void)
{
    assert(!strcmp(dbg_err_name(DBG_E_SHAPE), "shape") && !strcmp(dbg_err_name(DBG_E_RANGE), "range"));
    assert(!strcmp(dbg_err_name(DBG_E_BUSY), "busy") && !strcmp(dbg_err_name(DBG_E_MEMORY), "memory"));
    assert(!strcmp(dbg_err_name(DBG_E_ASLEEP), "asleep") && !strcmp(dbg_err_name(DBG_E_TIMEOUT), "timeout"));
    assert(!strcmp(dbg_err_name(DBG_E_TOUCH), "touch") && !strcmp(dbg_err_name(DBG_E_IO), "io"));
    assert(dbg_err_name(DBG_OK) && dbg_err_name((dbg_err_t)99));
}

int main(void)
{
    test_crc32();
    test_chunking();
    test_memory_policy();
    test_face();
    test_make();
    test_sampler();
    test_inject();
    test_inject_threads();
    test_names();
    puts("debug commands: PASS (crc32, 425-chunk screenshot round trip and loss detection, memory policy, round-face geometry, request validation, gesture sampler, task hand-off incl. threads)");
}
