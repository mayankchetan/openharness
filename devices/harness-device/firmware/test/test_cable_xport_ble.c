// B4b: the BLE transport of the phone mode (cable_xport_ble.c): a byte stream over an L2CAP channel, behind the cable_xport.h contract.
//
//   cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g -pthread -fsanitize=address,undefined -DCABLE_XPORT_BLE_WRITE_CAP_MS=300
//      -I host_tls -I ../main -o t test_cable_xport_ble.c ../main/cable_xport_ble.c
//
// The NimBLE side is a fake port (a struct of function pointers); everything under test is the logic above it: the receive ring and its credit rule
// (re-arm only while a whole SDU still fits), SDUs split and joined, the all-or-nothing write with its cap, STALLED / BUSY / NOMEM, and the close and
// reopen rules. The write cap is built short (300 ms) so the stall cases run fast; test_ble_link_source.py pins the real value (2000 ms, the
// `write_cap_ms` of the vector file).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cable_xport_ble.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); abort(); } checks++; } while (0)
#define CAP CABLE_XPORT_BLE_WRITE_CAP_MS

// the hooks host_tls/ wants from a test
void (*host_tls_delay_hook)(void);
void host_tls_log(char level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag; (void)fmt;
}

static int64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ── the fake port ───────────────────────────────────────────────────────────────────────────────────
#define MAX_SDUS 8192
typedef struct {
    pthread_mutex_t lock;
    uint8_t  *wire;                      // every byte the stack took, in order (1 MB)
    size_t   wire_len;
    size_t   sdu_len[MAX_SDUS];
    unsigned sdus;
    cable_xport_ble_send_t script[64];   // answers to send(), one per call, then default_result
    unsigned script_len, script_pos;
    cable_xport_ble_send_t default_result;
    size_t   mtu;
    bool     report_mtu_zero;            // peer_mtu() says 0 (the transport must still send legal SDUs); `mtu` is then what the stack accepts
    unsigned rearms, disconnects, send_calls, armed;
    bool     unstall_inline;             // a STALLED answer is followed by the unstall event BEFORE send() returns
    bool     stalled_open;               // an SDU was STALLED and has not been unstalled
    bool     violation;                  // sent past a stall (only BUSY retries are legal), or an SDU over the MTU or empty
} fake_t;
static fake_t F = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void fake_reset(void)
{
    pthread_mutex_lock(&F.lock);
    if (!F.wire) F.wire = malloc(1 << 20);
    F.wire_len = 0;
    F.sdus = 0;
    F.script_len = F.script_pos = 0;
    F.default_result = BLE_SEND_OK;
    F.mtu = CABLE_XPORT_BLE_SDU_MAX;
    F.report_mtu_zero = false;
    F.rearms = F.disconnects = F.send_calls = F.armed = 0;
    F.unstall_inline = F.stalled_open = F.violation = false;
    pthread_mutex_unlock(&F.lock);
}
static void script(unsigned n, ...)
{
    va_list ap;
    va_start(ap, n);
    pthread_mutex_lock(&F.lock);
    F.script_len = n;
    F.script_pos = 0;
    for (unsigned i = 0; i < n; i++) F.script[i] = (cable_xport_ble_send_t)va_arg(ap, int);
    pthread_mutex_unlock(&F.lock);
    va_end(ap);
}

static cable_xport_ble_send_t fake_send(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    pthread_mutex_lock(&F.lock);
    F.send_calls++;
    const cable_xport_ble_send_t r = F.script_pos < F.script_len ? F.script[F.script_pos++] : F.default_result;
    if (F.stalled_open && r != BLE_SEND_BUSY) F.violation = true;
    if (len > F.mtu || len == 0) F.violation = true;
    bool inline_unstall = false;
    if (r == BLE_SEND_OK || r == BLE_SEND_STALLED) {
        assert(F.wire_len + len <= (1 << 20) && F.sdus < MAX_SDUS);
        memcpy(F.wire + F.wire_len, data, len);
        F.wire_len += len;
        F.sdu_len[F.sdus++] = len;
        if (r == BLE_SEND_STALLED) {
            if (F.unstall_inline) inline_unstall = true; else F.stalled_open = true;
        }
    }
    pthread_mutex_unlock(&F.lock);
    if (inline_unstall) cable_xport_ble_on_tx_unstalled();
    return r;
}
static void fake_rearm(void *ctx) { (void)ctx; pthread_mutex_lock(&F.lock); F.rearms++; F.armed++; pthread_mutex_unlock(&F.lock); }
static void fake_disconnect(void *ctx) { (void)ctx; pthread_mutex_lock(&F.lock); F.disconnects++; pthread_mutex_unlock(&F.lock); }
static size_t fake_mtu(void *ctx) { (void)ctx; return F.report_mtu_zero ? 0 : F.mtu; }
static void unstall(void)   // the stack's TX_UNSTALLED event
{
    pthread_mutex_lock(&F.lock);
    F.stalled_open = false;
    pthread_mutex_unlock(&F.lock);
    cable_xport_ble_on_tx_unstalled();
}
static const cable_xport_ble_port_t PORT = {NULL, fake_send, fake_rearm, fake_disconnect, fake_mtu};

static const cable_xport_t *up(void)
{
    fake_reset();
    CHECK(cable_xport_ble_init(&PORT));
    cable_xport_ble_channel_up();
    const cable_xport_t *x = cable_xport_ble();
    CHECK(x->is_up(x) && x->open(x));
    return x;
}
static void down(void) { cable_xport_ble_deinit(); }

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) { s = s * 1664525u + 1013904223u; p[i] = (uint8_t)(s >> 24); }
}
static size_t read_all(const cable_xport_t *x, uint8_t *out, size_t want)
{
    size_t n = 0;
    while (n < want) { const int r = x->read(x, out + n, want - n, 0); CHECK(r > 0); n += (size_t)r; }
    return n;
}

// ── tests ───────────────────────────────────────────────────────────────────────────────────────────
static void test_shape_and_the_state_before_init(void)
{
    const cable_xport_t *x = cable_xport_ble();
    CHECK(x && x->kind == CABLE_XPORT_BLE && !strcmp(x->name, "ble") && x->inner == NULL);
    CHECK(x->open && x->close && x->read && x->write && x->is_up);
    uint8_t b[8] = {0};
    CHECK(!x->is_up(x) && x->read(x, b, sizeof b, 0) < 0 && x->write(x, b, sizeof b, 0) < 0 && !x->open(x));
    x->close(x);
    cable_xport_ble_channel_up();
    cable_xport_ble_channel_down();
    CHECK(!cable_xport_ble_on_sdu(b, sizeof b));
    cable_xport_ble_on_tx_unstalled();
    CHECK(!cable_xport_ble_init(NULL));
    const cable_xport_ble_port_t incomplete = {NULL, fake_send, NULL, fake_disconnect, fake_mtu};
    CHECK(!cable_xport_ble_init(&incomplete));
    CHECK(CABLE_XPORT_BLE_SDU_MAX == 4032 && CABLE_XPORT_BLE_RING >= 2 * CABLE_XPORT_BLE_SDU_MAX);
    // a channel that is not up yet does not take bytes
    fake_reset();
    CHECK(cable_xport_ble_init(&PORT));
    CHECK(!x->is_up(x) && !x->open(x) && !cable_xport_ble_on_sdu(b, sizeof b) && x->read(x, b, sizeof b, 0) < 0 && x->write(x, b, 4, 0) < 0);
    down();
}

static void test_sdus_split_and_joined(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[CABLE_XPORT_BLE_SDU_MAX], got[CABLE_XPORT_BLE_SDU_MAX];
    fill(src, sizeof src, 1);
    for (int i = 0; i < 300; i++) CHECK(cable_xport_ble_on_sdu(src + i, 1));                // one byte per SDU
    size_t n = 0;
    while (n < 300) { const int r = x->read(x, got + n, 7, 0); CHECK(r > 0 && r <= 7); n += (size_t)r; }
    CHECK(n == 300 && !memcmp(got, src, 300));
    CHECK(x->read(x, got, sizeof got, 0) == 0);                                              // a poll with nothing there
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX));                             // a whole SDU, read back in growing pieces
    n = 0;
    size_t step = 1;
    while (n < CABLE_XPORT_BLE_SDU_MAX) {
        const int r = x->read(x, got + n, step, 0);
        CHECK(r > 0);
        n += (size_t)r;
        step = step * 3 + 1;
    }
    CHECK(n == CABLE_XPORT_BLE_SDU_MAX && !memcmp(got, src, n));
    CHECK(cable_xport_ble_on_sdu(src, 100) && cable_xport_ble_on_sdu(src + 100, 200) && cable_xport_ble_on_sdu(src + 300, 300));   // joined in one read
    CHECK(x->read(x, got, sizeof got, 0) == 600 && !memcmp(got, src, 600));
    CHECK(cable_xport_ble_on_sdu(src, 0) && x->read(x, got, sizeof got, 0) == 0);          // an empty SDU is nothing
    down();
}

static void test_a_megabyte_wraps_the_ring_in_order(void)
{
    const cable_xport_t *x = up();
    static uint8_t big[1 << 20], out[1 << 20];
    fill(big, sizeof big, 2);
    uint32_t seed = 7;
    size_t fed = 0, got = 0;
    while (got < sizeof big) {
        seed = seed * 1103515245u + 12345u;
        const size_t chunk = 1 + (seed >> 8) % 1500;
        // the stack never has more than one SDU ahead of the ring's room: feed only while a whole SDU of ours still fits, as the credit rule makes true
        if (fed < sizeof big && fed - got + chunk <= CABLE_XPORT_BLE_RING) {
            const size_t m = chunk < sizeof big - fed ? chunk : sizeof big - fed;
            CHECK(cable_xport_ble_on_sdu(big + fed, m));
            fed += m;
        }
        seed = seed * 1103515245u + 12345u;
        const size_t want = 1 + (seed >> 8) % 1500;
        const int r = x->read(x, out + got, want, 0);
        CHECK(r >= 0);
        got += (size_t)r;
    }
    CHECK(!memcmp(big, out, sizeof big));
    down();
}

typedef struct { const uint8_t *data; size_t len; unsigned delay_ms; } feed_t;
static void *feeder(void *arg) { feed_t *f = arg; usleep(f->delay_ms * 1000); cable_xport_ble_on_sdu(f->data, f->len); return NULL; }
static void *closer(void *arg) { feed_t *f = arg; usleep(f->delay_ms * 1000); cable_xport_ble_channel_down(); return NULL; }

static void test_read_waits_wakes_and_ends(void)
{
    const cable_xport_t *x = up();
    uint8_t b[16];
    int64_t t = ms_now();
    CHECK(x->read(x, b, sizeof b, 0) == 0 && ms_now() - t < 30);                             // a poll never blocks
    t = ms_now();
    CHECK(x->read(x, b, sizeof b, 80) == 0);
    CHECK(ms_now() - t >= 75 && ms_now() - t < 500);                                         // waits its time, then says 0
    static const uint8_t hello[] = "hello";
    feed_t f = { hello, 5, 40 };
    pthread_t th;
    pthread_create(&th, NULL, feeder, &f);
    t = ms_now();
    CHECK(x->read(x, b, sizeof b, 3000) == 5 && ms_now() - t < 600 && !memcmp(b, "hello", 5));   // data wakes it early
    pthread_join(th, NULL);
    pthread_create(&th, NULL, closer, &f);
    t = ms_now();
    CHECK(x->read(x, b, sizeof b, 3000) < 0 && ms_now() - t < 600);                          // the channel closing wakes it with -1
    pthread_join(th, NULL);
    CHECK(!x->is_up(x));
    down();
    x = up();                                                                                // bytes that came before the close are still read, then -1
    CHECK(cable_xport_ble_on_sdu(hello, 5));
    cable_xport_ble_channel_down();
    CHECK(x->read(x, b, 3, 0) == 3 && x->read(x, b, 16, 0) == 2 && x->read(x, b, 16, 0) < 0 && x->read(x, b, 16, 50) < 0);
    down();
}

static void test_the_credit_rule_rearms_only_with_room_for_a_whole_sdu(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[CABLE_XPORT_BLE_SDU_MAX], got[CABLE_XPORT_BLE_RING];
    fill(src, sizeof src, 3);
    // an empty ring: each SDU is followed at once by a new receive buffer
    CHECK(cable_xport_ble_on_sdu(src, 500) && F.rearms == 1);
    CHECK(cable_xport_ble_on_sdu(src, 500) && F.rearms == 2);
    CHECK(x->read(x, got, sizeof got, 0) == 1000 && F.rearms == 2);                          // nothing was pending: reading re-arms nothing
    // two whole SDUs: the first leaves 4,160 free (room for one more, so re-arm), the second leaves 128 (no room: pending)
    cable_xport_ble_channel_up();
    fake_reset();
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX) && F.rearms == 1);
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX) && F.rearms == 1);
    // 8,064 held. A whole SDU fits again at 4,160 held, i.e. after 3,904 bytes are read: 3,903 is one byte short, 3,904 is enough
    CHECK(read_all(x, got, 3903) == 3903 && F.rearms == 1);
    CHECK(x->read(x, got, 1, 0) == 1 && F.rearms == 2);
    CHECK(x->read(x, got, sizeof got, 0) == 4160 && F.rearms == 2);                          // the pending re-arm went once; more reading adds none
    // a second pending re-arm is separate: fill again, drain in one read
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX) && cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX));
    CHECK(F.rearms == 3);                                                                    // the first of the two re-armed (empty ring), the second did not
    CHECK(x->read(x, got, sizeof got, 0) == 2 * CABLE_XPORT_BLE_SDU_MAX && F.rearms == 4);
    // a channel that went down while a re-arm was pending is not re-armed
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX) && cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX));
    const unsigned before = F.rearms;
    cable_xport_ble_channel_down();
    CHECK(x->read(x, got, sizeof got, 0) > 0 && F.rearms == before);
    down();
}

static void test_a_peer_that_sends_past_its_credits_breaks_the_stream(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[CABLE_XPORT_BLE_SDU_MAX];
    fill(src, sizeof src, 4);
    CHECK(cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX) && cable_xport_ble_on_sdu(src, CABLE_XPORT_BLE_SDU_MAX));   // 128 B free
    CHECK(!cable_xport_ble_on_sdu(src, 200));                                                 // does not fit: false, and the channel is closed
    CHECK(F.disconnects == 1 && !x->is_up(x));
    uint8_t b[16];
    CHECK(x->read(x, b, sizeof b, 0) < 0);                                                    // a broken stream gives nothing more, not even what was in the ring
    CHECK(!cable_xport_ble_on_sdu(src, 1) && x->write(x, b, 4, 0) < 0);
    cable_xport_ble_channel_up();                                                             // a new channel starts clean
    CHECK(x->is_up(x) && x->read(x, b, sizeof b, 0) == 0 && cable_xport_ble_on_sdu(src, 10) && x->read(x, b, sizeof b, 0) == 10);
    down();
}

static void test_write_splits_to_the_peer_mtu(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[10000];
    fill(src, sizeof src, 5);
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src);
    CHECK(F.sdus == 3 && F.sdu_len[0] == 4032 && F.sdu_len[1] == 4032 && F.sdu_len[2] == 1936);
    CHECK(F.wire_len == sizeof src && !memcmp(F.wire, src, sizeof src) && !F.violation);
    fake_reset();
    F.mtu = 504;                                                                              // a phone with a small SDU MTU
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src && F.sdus == (sizeof src + 503) / 504 && !F.violation && !memcmp(F.wire, src, sizeof src));
    for (unsigned i = 0; i + 1 < F.sdus; i++) CHECK(F.sdu_len[i] == 504);
    fake_reset();
    F.mtu = 100000;                                                                           // a phone that allows more than we do: still 4,032 at most
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src && F.sdu_len[0] == 4032 && !F.violation);
    fake_reset();
    F.mtu = 23;
    F.report_mtu_zero = true;                                                                 // an MTU that cannot be read: 23-byte SDUs
    CHECK(x->write(x, src, 100, 0) == 100 && F.sdus == 5 && F.sdu_len[0] == 23 && F.sdu_len[4] == 8 && !F.violation);
    fake_reset();
    CHECK(x->write(x, src, 1, 0) == 1 && F.sdus == 1 && F.wire[0] == src[0]);
    CHECK(x->write(x, src, 0, 0) == 0 && F.sdus == 1);                                        // nothing to send: nothing taken
    CHECK(x->is_up(x));
    down();
}

static void *unstaller(void *arg) { usleep(((feed_t *)arg)->delay_ms * 1000); unstall(); return NULL; }

static void test_a_stalled_sdu_waits_for_the_unstall_event(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[3000];
    fill(src, sizeof src, 6);
    F.mtu = 1000;
    script(3, BLE_SEND_OK, BLE_SEND_STALLED, BLE_SEND_OK);
    feed_t f = { NULL, 0, 60 };
    pthread_t th;
    pthread_create(&th, NULL, unstaller, &f);
    const int64_t t = ms_now();
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src);
    CHECK(ms_now() - t >= 55 && ms_now() - t < CAP);                                          // it waited for the event, not for the cap
    pthread_join(th, NULL);
    CHECK(F.sdus == 3 && !F.violation && !memcmp(F.wire, src, sizeof src) && F.disconnects == 0 && x->is_up(x));
    // the event that comes before send() has returned is not lost
    fake_reset();
    F.mtu = 1000;
    F.unstall_inline = true;
    script(3, BLE_SEND_STALLED, BLE_SEND_STALLED, BLE_SEND_OK);
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src && F.sdus == 3 && !F.violation);
    // an event from an earlier stall does not release a new one
    fake_reset();
    F.mtu = 1000;
    cable_xport_ble_on_tx_unstalled();
    script(2, BLE_SEND_STALLED, BLE_SEND_OK);
    f.delay_ms = 80;
    pthread_create(&th, NULL, unstaller, &f);
    const int64_t t2 = ms_now();
    CHECK(x->write(x, src, 2000, 0) == 2000 && ms_now() - t2 >= 75);
    pthread_join(th, NULL);
    down();
}

static void test_a_stall_that_never_clears_closes_the_channel(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[3000];
    fill(src, sizeof src, 7);
    F.mtu = 1000;
    script(2, BLE_SEND_OK, BLE_SEND_STALLED);
    int64_t t = ms_now();
    CHECK(x->write(x, src, sizeof src, 0) == -1);                                             // taken some, then stuck: the link dies
    CHECK(ms_now() - t >= CAP - 50 && ms_now() - t < CAP + 1500);
    CHECK(F.disconnects == 1 && !x->is_up(x));
    CHECK(x->write(x, src, 10, 0) == -1);                                                     // and stays dead until the stack reopens a channel
    down();
    // the first SDU stalled and never cleared: it was taken, so this is the same
    x = up();
    script(1, BLE_SEND_STALLED);
    CHECK(x->write(x, src, 100, 0) == -1 && F.disconnects == 1);
    down();
}

static void test_busy_and_nomem_with_nothing_taken_return_zero(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[500];
    fill(src, sizeof src, 8);
    F.default_result = BLE_SEND_BUSY;
    int64_t t = ms_now();
    CHECK(x->write(x, src, sizeof src, 0) == 0);                                              // nothing taken in the whole cap: the link is fine
    CHECK(ms_now() - t >= CAP - 50 && ms_now() - t < CAP + 1500 && F.disconnects == 0 && x->is_up(x));
    F.default_result = BLE_SEND_OK;
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src && !memcmp(F.wire, src, sizeof src));   // the same buffer, retried, goes through
    fake_reset();
    F.default_result = BLE_SEND_NOMEM;
    t = ms_now();
    CHECK(x->write(x, src, sizeof src, 0) == 0 && ms_now() - t >= CAP - 50 && F.disconnects == 0 && x->is_up(x) && F.wire_len == 0);
    // BUSY and NOMEM that clear in time: the same SDU is offered again, and nothing is sent twice
    fake_reset();
    script(5, BLE_SEND_BUSY, BLE_SEND_BUSY, BLE_SEND_NOMEM, BLE_SEND_NOMEM, BLE_SEND_OK);
    CHECK(x->write(x, src, sizeof src, 0) == (int)sizeof src && F.sdus == 1 && F.send_calls == 5 && !memcmp(F.wire, src, sizeof src));
    down();
}

static void test_busy_and_nomem_after_some_bytes_close_the_channel(void)
{
    static uint8_t src[3000];
    fill(src, sizeof src, 9);
    const cable_xport_t *x = up();
    F.mtu = 1000;
    script(1, BLE_SEND_OK);
    F.default_result = BLE_SEND_BUSY;                                                         // the first SDU went; then the stack stays busy
    CHECK(x->write(x, src, sizeof src, 0) == -1 && F.disconnects == 1 && !x->is_up(x) && F.sdus == 1);
    down();
    x = up();
    F.mtu = 1000;
    script(2, BLE_SEND_OK, BLE_SEND_OK);
    F.default_result = BLE_SEND_NOMEM;
    CHECK(x->write(x, src, sizeof src, 0) == -1 && F.disconnects == 1 && F.sdus == 2);
    down();
}

static void test_an_error_closes_the_channel(void)
{
    static uint8_t src[3000];
    fill(src, sizeof src, 10);
    const cable_xport_t *x = up();
    script(1, BLE_SEND_ERR);
    CHECK(x->write(x, src, 100, 0) == -1 && F.disconnects == 1 && !x->is_up(x));
    down();
    x = up();
    F.mtu = 1000;
    script(2, BLE_SEND_OK, BLE_SEND_ERR);
    CHECK(x->write(x, src, sizeof src, 0) == -1 && F.disconnects == 1 && F.sdus == 1);
    down();
    // the channel closing under a writer that is waiting for an unstall ends the write at once
    x = up();
    F.mtu = 1000;
    script(1, BLE_SEND_STALLED);
    feed_t f = { NULL, 0, 60 };
    pthread_t th;
    pthread_create(&th, NULL, closer, &f);
    const int64_t t = ms_now();
    CHECK(x->write(x, src, 2000, 0) == -1 && ms_now() - t < CAP);
    pthread_join(th, NULL);
    down();
}

typedef struct { const cable_xport_t *x; const uint8_t *src; int result; } wr_t;
static void *writer(void *arg) { wr_t *w = arg; w->result = w->x->write(w->x, w->src, 2000, 0); return NULL; }

static void test_one_writer_at_a_time(void)
{
    const cable_xport_t *x = up();
    static uint8_t src[2000];
    fill(src, sizeof src, 11);
    F.mtu = 1000;
    script(1, BLE_SEND_STALLED);
    wr_t w = { x, src, 99 };
    pthread_t th;
    pthread_create(&th, NULL, writer, &w);
    usleep(50000);
    CHECK(x->write(x, src, 10, 0) == 0);                                                      // a second write while one is in flight takes nothing
    unstall();
    pthread_join(th, NULL);
    CHECK(w.result == 2000 && F.wire_len == 2000);
    CHECK(x->write(x, src, 10, 0) == 10);
    down();
}

static void test_close_and_reopen(void)
{
    const cable_xport_t *x = up();
    uint8_t b[32];
    static const uint8_t stale[] = "stale bytes";
    CHECK(cable_xport_ble_on_sdu(stale, sizeof stale));
    x->close(x);
    CHECK(F.disconnects == 1 && !x->is_up(x) && !x->open(x));
    x->close(x);
    CHECK(F.disconnects == 1);                                                                // once
    CHECK(x->write(x, b, 4, 0) < 0);
    cable_xport_ble_channel_down();                                                           // the stack's answer changes nothing
    cable_xport_ble_channel_up();                                                             // a new channel: clean ring, up again, open() true
    CHECK(x->is_up(x) && x->open(x) && x->read(x, b, sizeof b, 0) == 0);                      // none of the old session leaks into the new
    CHECK(x->write(x, (const uint8_t *)"ok", 2, 0) == 2);
    down();
}

typedef struct { const cable_xport_t *x; uint8_t *src; size_t total; int bad; } pump_t;
static void *stack_rx(void *arg)   // the stack: feeds SDUs, but only into buffers it has been armed with
{
    pump_t *p = arg;
    size_t fed = 0;
    uint32_t seed = 99;
    while (fed < p->total) {
        pthread_mutex_lock(&F.lock);
        const bool armed = F.armed > 0;
        if (armed) F.armed--;
        pthread_mutex_unlock(&F.lock);
        if (!armed) { usleep(200); continue; }
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 8) % CABLE_XPORT_BLE_SDU_MAX;
        if (n > p->total - fed) n = p->total - fed;
        if (!cable_xport_ble_on_sdu(p->src + fed, n)) { p->bad++; return NULL; }
        fed += n;
    }
    return NULL;
}
static void *slow_reader(void *arg)
{
    pump_t *p = arg;
    static uint8_t out[1 << 20];
    size_t got = 0;
    uint32_t seed = 5;
    while (got < p->total) {
        seed = seed * 1103515245u + 12345u;
        const int r = p->x->read(p->x, out + got, 1 + (seed >> 8) % 3000, 50);
        if (r < 0) { p->bad++; return NULL; }
        got += (size_t)r;
        if ((seed >> 20) % 16 == 0) usleep(300);                                              // now and then the reader falls behind: credits must hold the stack back
    }
    if (memcmp(out, p->src, p->total)) p->bad++;
    return NULL;
}
static void *sender(void *arg)
{
    pump_t *p = arg;
    size_t sent = 0;
    uint32_t seed = 1234;
    while (sent < p->total) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 8) % 9000;
        if (n > p->total - sent) n = p->total - sent;
        int r;
        while ((r = p->x->write(p->x, p->src + sent, n, 0)) == 0) usleep(100);
        if (r != (int)n) { p->bad++; return NULL; }
        sent += n;
    }
    return NULL;
}

static void test_reader_writer_and_the_stack_at_once(void)
{
    const cable_xport_t *x = up();
    static uint8_t rx_src[600000], tx_src[600000];
    fill(rx_src, sizeof rx_src, 12);
    fill(tx_src, sizeof tx_src, 13);
    F.armed = 1;                                                                              // the channel opens with one receive buffer armed
    pump_t stack = { x, rx_src, sizeof rx_src, 0 }, reader = { x, rx_src, sizeof rx_src, 0 }, tx = { x, tx_src, sizeof tx_src, 0 };
    pthread_t a, b, c;
    pthread_create(&a, NULL, stack_rx, &stack);
    pthread_create(&b, NULL, slow_reader, &reader);
    pthread_create(&c, NULL, sender, &tx);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    CHECK(stack.bad == 0 && reader.bad == 0 && tx.bad == 0);                                  // nothing dropped, nothing out of order, no deadlock
    CHECK(F.wire_len == sizeof tx_src && !memcmp(F.wire, tx_src, sizeof tx_src) && !F.violation);
    CHECK(F.disconnects == 0);
    down();
}

static void test_init_and_deinit_balance(void)
{
    fake_reset();
    CHECK(cable_xport_ble_init(&PORT));
    CHECK(cable_xport_ble_init(&PORT));                                                       // idempotent: no second ring
    cable_xport_ble_deinit();
    cable_xport_ble_deinit();                                                                 // and safe twice
    const struct mallinfo2 before = mallinfo2();
    for (int i = 0; i < 200; i++) {
        CHECK(cable_xport_ble_init(&PORT));
        cable_xport_ble_channel_up();
        CHECK(cable_xport_ble_on_sdu((const uint8_t *)"x", 1));
        cable_xport_ble_channel_down();
        cable_xport_ble_deinit();
        CHECK(!cable_xport_ble()->is_up(cable_xport_ble()));
    }
    const struct mallinfo2 after = mallinfo2();
    CHECK(after.uordblks == before.uordblks);                                                 // 200 cycles, not a byte kept
}

int main(void)
{
    test_shape_and_the_state_before_init();
    test_sdus_split_and_joined();
    test_a_megabyte_wraps_the_ring_in_order();
    test_read_waits_wakes_and_ends();
    test_the_credit_rule_rearms_only_with_room_for_a_whole_sdu();
    test_a_peer_that_sends_past_its_credits_breaks_the_stream();
    test_write_splits_to_the_peer_mtu();
    test_a_stalled_sdu_waits_for_the_unstall_event();
    test_a_stall_that_never_clears_closes_the_channel();
    test_busy_and_nomem_with_nothing_taken_return_zero();
    test_busy_and_nomem_after_some_bytes_close_the_channel();
    test_an_error_closes_the_channel();
    test_one_writer_at_a_time();
    test_close_and_reopen();
    test_reader_writer_and_the_stack_at_once();
    test_init_and_deinit_balance();
    printf("cable_xport_ble: %u checks PASS\n", checks);
    return 0;
}
