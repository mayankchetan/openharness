// W11: the log ring (log_ring.c) that carries CABLE_TYPE_LOG lines to the Wi-Fi link's net task. Finding F2 of the Wi-Fi spec: the log hook runs in
// every task, so it must never call a socket or TLS; it puts the line in this ring and returns. The ring drops its OLDEST lines when full and
// never waits for the consumer. Tested single-threaded for exact content, then with four producer threads and one consumer.
//
//   cc -std=c11 -Wall -Wextra -Werror -O1 -g -pthread -fsanitize=undefined,bounds -I ../main test_log_ring.c ../main/log_ring.c
//   (the same file builds under -fsanitize=thread)
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log_ring.h"

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

// ── single thread: exact content ───────────────────────────────────────────────────────────────────

static size_t pop_str(log_ring_t *r, char *out)
{
    uint8_t buf[LOG_RING_LINE_MAX];
    const size_t n = log_ring_pop(r, buf, sizeof buf);
    memcpy(out, buf, n);
    out[n] = 0;
    return n;
}

static void test_basic(void)
{
    uint8_t storage[64];
    log_ring_t r;
    log_ring_init(&r, storage, sizeof storage);
    char s[LOG_RING_LINE_MAX + 1];
    CHECK(pop_str(&r, s) == 0 && log_ring_used(&r) == 0);   // empty
    CHECK(log_ring_push(&r, (const uint8_t *)"hello", 5) && log_ring_used(&r) == 2 + 5);
    CHECK(log_ring_push(&r, (const uint8_t *)"x", 1));
    CHECK(pop_str(&r, s) == 5 && !strcmp(s, "hello"));
    CHECK(pop_str(&r, s) == 1 && !strcmp(s, "x"));
    CHECK(pop_str(&r, s) == 0 && log_ring_used(&r) == 0 && log_ring_overwritten(&r) == 0);
    CHECK(!log_ring_push(&r, (const uint8_t *)"", 0) && !log_ring_push(&r, NULL, 3));   // nothing to say is not a line
    // Drop-oldest, exact: a 9-byte line takes 11 bytes with its length, so five fit in 64. Push eight: lines 0 to 2 go, 3 to 7 stay, in order.
    for (int i = 0; i < 8; i++) {
        char line[16];
        snprintf(line, sizeof line, "line-%04d", i);   // 9 chars
        CHECK(log_ring_push(&r, (const uint8_t *)line, 9));
    }
    CHECK(log_ring_overwritten(&r) == 8 - 5);
    for (int i = 3; i < 8; i++) {
        char want[16];
        snprintf(want, sizeof want, "line-%04d", i);
        CHECK(pop_str(&r, s) == 9 && !strcmp(s, want));
    }
    CHECK(pop_str(&r, s) == 0);
    // The positions wrap many times without losing a byte.
    log_ring_init(&r, storage, sizeof storage);
    for (int i = 0; i < 1000; i++) {
        char line[40];
        const int n = snprintf(line, sizeof line, "wrap %d %.*s", i, i % 17, "abcdefghijklmnopqrstuvwxyz");
        CHECK(log_ring_push(&r, (const uint8_t *)line, (size_t)n));
        CHECK(pop_str(&r, s) == (size_t)n && !memcmp(s, line, (size_t)n));
    }
    CHECK(log_ring_overwritten(&r) == 0);
}

static void test_limits(void)
{
    // A line longer than LOG_RING_LINE_MAX is cut, not refused and not split.
    uint8_t storage[4096];
    log_ring_t r;
    log_ring_init(&r, storage, sizeof storage);
    uint8_t big[LOG_RING_LINE_MAX + 200];
    memset(big, 'z', sizeof big);
    CHECK(log_ring_push(&r, big, sizeof big));
    uint8_t out[LOG_RING_LINE_MAX];
    CHECK(log_ring_pop(&r, out, sizeof out) == LOG_RING_LINE_MAX && out[0] == 'z' && out[LOG_RING_LINE_MAX - 1] == 'z');
    // A consumer buffer that is too small gets nothing and the line stays queued.
    CHECK(log_ring_push(&r, (const uint8_t *)"abc", 3));
    CHECK(log_ring_pop(&r, out, 2) == 0 && log_ring_used(&r) == 5);
    CHECK(log_ring_pop(&r, out, sizeof out) == 3);
    // A ring smaller than one entry refuses everything and never touches memory.
    uint8_t tiny[2];
    log_ring_init(&r, tiny, sizeof tiny);
    CHECK(!log_ring_push(&r, (const uint8_t *)"a", 1) && log_ring_pop(&r, out, sizeof out) == 0);
    // A line that cannot fit even in an empty ring is refused and counted; the older lines are not sacrificed for it.
    uint8_t small[32];
    log_ring_init(&r, small, sizeof small);
    CHECK(log_ring_push(&r, (const uint8_t *)"keep", 4));
    uint8_t forty[40];
    memset(forty, 'q', sizeof forty);
    CHECK(!log_ring_push(&r, forty, sizeof forty) && log_ring_rejected(&r) == 1);
    CHECK(log_ring_pop(&r, out, sizeof out) == 4);
    // Never blocks, never grows: with no consumer at all, a hundred thousand lines go in and the ring holds the newest.
    log_ring_init(&r, storage, 1024);
    for (uint32_t i = 0; i < 100000; i++) {
        char line[24];
        const int n = snprintf(line, sizeof line, "n=%u", i);
        CHECK(log_ring_push(&r, (const uint8_t *)line, (size_t)n));
    }
    CHECK(log_ring_used(&r) <= 1024 && log_ring_overwritten(&r) > 99000);
    uint32_t last = 0, popped = 0;
    for (;;) {
        const size_t n = log_ring_pop(&r, out, sizeof out);
        if (!n) break;
        out[n] = 0;
        unsigned v = 0;
        CHECK(sscanf((const char *)out, "n=%u", &v) == 1 && (popped == 0 || v == last + 1));
        last = v;
        popped++;
    }
    CHECK(last == 99999 && popped + log_ring_overwritten(&r) == 100000);
}

// ── four producers, one consumer ───────────────────────────────────────────────────────────────────

#define PRODUCERS 4
#define LINES_EACH 40000

static log_ring_t ring;
static atomic_int producers_done;
static atomic_bool failed;

typedef struct { uint8_t who; uint32_t seq; uint8_t len; uint8_t sum; } hdr_t;   // 7 bytes; the rest is filler derived from seq

static uint8_t filler(uint32_t seq, unsigned i) { return (uint8_t)(seq * 31u + i * 7u + 3u); }

static void *produce(void *arg)
{
    const unsigned who = (unsigned)(uintptr_t)arg;
    uint32_t rng = 0x2545F491u + who * 7919u;
    for (uint32_t seq = 0; seq < LINES_EACH; seq++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const unsigned total = 8 + rng % 120;   // 8..127 bytes
        uint8_t line[128];
        line[0] = (uint8_t)who;
        memcpy(line + 1, &seq, 4);
        line[5] = (uint8_t)total;
        uint8_t sum = 0;
        for (unsigned i = 7; i < total; i++) { line[i] = filler(seq, i); sum = (uint8_t)(sum + line[i]); }
        line[6] = sum;
        if (!log_ring_push(&ring, line, total)) atomic_store(&failed, true);
        if (!(rng & 255)) sched_yield();
    }
    atomic_fetch_add(&producers_done, 1);
    return NULL;
}

static uint64_t popped_total;
static uint32_t last_seq[PRODUCERS];
static bool seen[PRODUCERS];

static void check_line(const uint8_t *line, size_t n)
{
    assert(n >= 8 && n == line[5]);
    const unsigned who = line[0];
    assert(who < PRODUCERS);
    uint32_t seq;
    memcpy(&seq, line + 1, 4);
    uint8_t sum = 0;
    for (unsigned i = 7; i < n; i++) { assert(line[i] == filler(seq, i)); sum = (uint8_t)(sum + line[i]); }
    assert(sum == line[6]);
    // Drop-oldest only ever removes from the front, so what survives from one producer is still in its own order.
    assert(!seen[who] || seq > last_seq[who]);
    seen[who] = true;
    last_seq[who] = seq;
    popped_total++;
}

static void *consume(void *arg)
{
    const bool slow = arg != NULL;
    uint8_t line[LOG_RING_LINE_MAX];
    for (;;) {
        const size_t n = log_ring_pop(&ring, line, sizeof line);
        if (n) {
            check_line(line, n);
            if (slow && !(popped_total & 3)) { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }   // falls behind: the ring must drop, not wait
            continue;
        }
        if (atomic_load(&producers_done) == PRODUCERS) {
            const size_t m = log_ring_pop(&ring, line, sizeof line);   // one last look after the producers stopped
            if (!m) return NULL;
            check_line(line, m);
        } else {
            sched_yield();
        }
    }
}

static void run_threads(uint32_t cap, int mode)   // mode 0: fast consumer, 1: slow consumer, 2: consumer starts after the producers
{
    uint8_t *storage = malloc(cap + 16);
    assert(storage);
    memset(storage, 0xA5, cap + 16);
    log_ring_init(&ring, storage + 8, cap);
    atomic_store(&producers_done, 0);
    atomic_store(&failed, false);
    popped_total = 0;
    memset(seen, 0, sizeof seen);
    memset(last_seq, 0, sizeof last_seq);
    pthread_t p[PRODUCERS], c;
    for (uintptr_t i = 0; i < PRODUCERS; i++) assert(!pthread_create(&p[i], NULL, produce, (void *)i));
    if (mode != 2) assert(!pthread_create(&c, NULL, consume, mode == 1 ? (void *)1 : NULL));
    for (int i = 0; i < PRODUCERS; i++) assert(!pthread_join(p[i], NULL));
    if (mode == 2) assert(!pthread_create(&c, NULL, consume, NULL));
    assert(!pthread_join(c, NULL));
    const uint64_t pushed = (uint64_t)PRODUCERS * LINES_EACH;
    CHECK(!atomic_load(&failed) && log_ring_rejected(&ring) == 0);
    // Every line is either delivered intact or counted as dropped: nothing vanishes, nothing is delivered twice.
    CHECK(popped_total + log_ring_overwritten(&ring) == pushed);
    CHECK(log_ring_used(&ring) == 0);
    for (int i = 0; i < 8; i++) CHECK(storage[i] == 0xA5 && storage[cap + 8 + i] == 0xA5);
    if (mode == 2) CHECK(log_ring_overwritten(&ring) > 0 && popped_total > 0);   // nobody was draining: it dropped, and did not stop the producers
    printf("  ring %u B, consumer %s: %llu delivered, %u dropped\n", cap, mode == 0 ? "fast" : mode == 1 ? "slow" : "late",
           (unsigned long long)popped_total, log_ring_overwritten(&ring));
    free(storage);
}

int main(void)
{
    test_basic();
    test_limits();
    run_threads(8192, 0);
    run_threads(8192, 1);
    run_threads(8192, 2);
    run_threads(600, 1);   // barely more than a few entries: constant dropping
    printf("log_ring: PASS (%u checks; exact drop-oldest, wrap, limits, %d producers x %d lines against one consumer)\n", checks, PRODUCERS, LINES_EACH);
    return 0;
}
