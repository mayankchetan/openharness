// W5: the frame decoder over a caller buffer, and the transport vtable (cable_xport.h) as a contract.
//
//   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=undefined,bounds test_cable_xport.c ../main/cable_frame.c
//
// Part 1 drives cable_decoder_buf_t (cap 1,024 and full size) against the fixed cable_decoder_t on the same
// bytes. Part 2 builds a loopback transport and a wrapper transport on top of it (an XOR stream standing in
// for TLS), to show the six operations compose: the wrapper uses the inner read/write the way the mbedTLS
// BIO callbacks will (cable_xport.h), answers partial inner writes by finishing the call, and reports the
// inner transport's kind. No TLS, Wi-Fi or BLE is involved.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_frame.h"
#include "cable_xport.h"

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

// ── decoder over a caller buffer ───────────────────────────────────────────────────────────────────

typedef struct { uint8_t type; size_t len; uint8_t first; } seen_t;
typedef struct { seen_t s[32]; unsigned n; } log_t;

static void collect(uint8_t version, uint8_t type, const uint8_t *p, size_t n, void *ctx)
{
    log_t *l = ctx;
    CHECK(version == CABLE_FRAME_VERSION);
    CHECK(l->n < 32);
    l->s[l->n++] = (seen_t){ type, n, n ? p[0] : 0 };
}

// payload bytes that cannot contain the magic pair: value 0x11 + (i % 7)
static void fill(uint8_t *p, size_t n, uint8_t salt)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(0x11 + ((i + salt) % 7));
}

static size_t put_frame(uint8_t *out, uint8_t type, size_t payload_len, uint8_t salt)
{
    uint8_t payload[CABLE_MAX_PAYLOAD];
    fill(payload, payload_len, salt);
    payload[0] = salt;   // lets the test tell frames apart
    int n = cable_frame_encode(type, payload, payload_len, out, CABLE_MAX_FRAME);
    CHECK(n == (int)(CABLE_HEADER_BYTES + payload_len + CABLE_CRC_BYTES));
    return (size_t)n;
}

#define GUARD 16
static void test_cap_1024(void)
{
    uint8_t *raw = malloc(1024 + 2 * GUARD);
    memset(raw, 0xEE, 1024 + 2 * GUARD);
    uint8_t *buf = raw + GUARD;
    cable_decoder_buf_t d;
    CHECK(cable_decoder_init_with(&d, buf, 1024));

    static uint8_t stream[4 * CABLE_MAX_FRAME];
    size_t at = 0;
    at += put_frame(stream + at, CABLE_TYPE_JSON, 20, 1);                                   // A
    const size_t big = put_frame(stream + at, CABLE_TYPE_PCM, 1025 - CABLE_HEADER_BYTES - CABLE_CRC_BYTES, 2);
    CHECK(big == 1025);                                                                     // does not fit in 1,024
    at += big;
    at += put_frame(stream + at, CABLE_TYPE_JSON, 30, 3);                                   // B
    const size_t exact = put_frame(stream + at, CABLE_TYPE_PCM, 1024 - CABLE_HEADER_BYTES - CABLE_CRC_BYTES, 4);
    CHECK(exact == 1024);                                                                   // fits exactly
    at += exact;
    at += put_frame(stream + at, CABLE_TYPE_JSON, 5, 5);                                    // C

    for (unsigned mode = 0; mode < 3; mode++) {   // bulk, byte at a time, 7-byte reads
        log_t l = {0};
        CHECK(cable_decoder_init_with(&d, buf, 1024));
        const size_t step = mode == 0 ? at : mode == 1 ? 1 : 7;
        for (size_t i = 0; i < at; i += step)
            cable_decoder_buf_feed(&d, stream + i, at - i < step ? at - i : step, collect, &l);
        CHECK(l.n == 4);
        CHECK(l.s[0].type == CABLE_TYPE_JSON && l.s[0].len == 20 && l.s[0].first == 1);
        CHECK(l.s[1].type == CABLE_TYPE_JSON && l.s[1].len == 30 && l.s[1].first == 3);
        CHECK(l.s[2].type == CABLE_TYPE_PCM && l.s[2].len == 1024 - CABLE_HEADER_BYTES - CABLE_CRC_BYTES && l.s[2].first == 4);
        CHECK(l.s[3].type == CABLE_TYPE_JSON && l.s[3].len == 5 && l.s[3].first == 5);
        CHECK(d.discarded_bytes == 1025);   // the whole oversize frame, once; nothing else
        CHECK(d.corrupt_frames == 0);
        CHECK(d.len == 0);
    }
    // The guard bytes around the caller's buffer were never touched.
    for (size_t i = 0; i < GUARD; i++) CHECK(raw[i] == 0xEE && raw[GUARD + 1024 + i] == 0xEE);
    free(raw);
}

static void test_init_with_rejects_bad_args(void)
{
    uint8_t b[64];
    cable_decoder_buf_t d;
    CHECK(!cable_decoder_init_with(&d, NULL, 1024));
    CHECK(!cable_decoder_init_with(&d, b, 0));
    CHECK(!cable_decoder_init_with(&d, b, CABLE_HEADER_BYTES + CABLE_CRC_BYTES - 1));
    CHECK(cable_decoder_init_with(&d, b, CABLE_HEADER_BYTES + CABLE_CRC_BYTES));
    // an empty frame is the smallest legal one and fits the smallest legal buffer
    uint8_t f[CABLE_MAX_FRAME];
    const int n = cable_frame_encode(CABLE_TYPE_JSON, NULL, 0, f, sizeof f);
    CHECK(n == CABLE_HEADER_BYTES + CABLE_CRC_BYTES);
    log_t l = {0};
    cable_decoder_buf_feed(&d, f, (size_t)n, collect, &l);
    CHECK(l.n == 1 && l.s[0].len == 0);
}

// Same bytes, same output: the fixed decoder, a full-size caller buffer, and every split of a noisy stream.
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_equivalent_to_fixed(void)
{
    static uint8_t stream[6 * CABLE_MAX_FRAME];
    size_t at = 0;
    for (unsigned i = 0; i < 40 && at < sizeof stream - CABLE_MAX_FRAME; i++) {
        const unsigned kind = rnd() % 4;
        if (kind == 0) {                                   // noise, including bare magic bytes
            const size_t n = 1 + rnd() % 40;
            for (size_t j = 0; j < n; j++) stream[at++] = (uint8_t)(rnd() % 3 == 0 ? CABLE_MAGIC_0 : rnd());
        } else if (kind == 1) {                            // a frame with a bad CRC
            at += put_frame(stream + at, CABLE_TYPE_JSON, rnd() % 200, (uint8_t)i);
            stream[at - 1] ^= 0x55;
        } else {
            at += put_frame(stream + at, kind == 2 ? CABLE_TYPE_JSON : CABLE_TYPE_PCM, rnd() % 3000, (uint8_t)i);
        }
    }
    for (unsigned step = 1; step < 600; step = step * 2 + 1) {
        static cable_decoder_t fixed;
        static uint8_t buf[CABLE_MAX_FRAME];
        cable_decoder_buf_t d;
        log_t a = {0}, b = {0};
        cable_decoder_init(&fixed);
        CHECK(cable_decoder_init_with(&d, buf, sizeof buf));
        for (size_t i = 0; i < at; i += step) {
            const size_t n = at - i < step ? at - i : step;
            cable_decoder_feed(&fixed, stream + i, n, collect, &a);
            cable_decoder_buf_feed(&d, stream + i, n, collect, &b);
            CHECK(fixed.len == d.len && !memcmp(fixed.buf, buf, d.len));
        }
        CHECK(a.n == b.n && !memcmp(a.s, b.s, sizeof a.s[0] * a.n));
        CHECK(fixed.discarded_bytes == d.discarded_bytes && fixed.corrupt_frames == d.corrupt_frames);
        CHECK(a.n > 0);
    }
}

// cable_link_reset_decoder() is called from the frame callback. Both flavours must survive a reset there.
typedef struct { void *d; int fixed; unsigned frames; } reset_ctx_t;
static void reset_in_cb(uint8_t v, uint8_t t, const uint8_t *p, size_t n, void *ctx)
{
    (void)v; (void)t; (void)p; (void)n;
    reset_ctx_t *r = ctx;
    r->frames++;
    if (r->fixed) cable_decoder_reset(r->d); else cable_decoder_buf_reset(r->d);
}

static void count_cb(uint8_t v, uint8_t t, const uint8_t *p, size_t n, void *ctx)
{
    (void)v; (void)t; (void)p; (void)n;
    reset_ctx_t *r = ctx;
    cable_decoder_t *f = r->d;
    CHECK(f->discarded_bytes == 5 && f->len > 0);   // synced to the public fields before the callback
    r->frames++;
    cable_decoder_reset(f);
}

static void test_reset_in_callback(void)
{
    uint8_t two[2 * CABLE_MAX_FRAME];
    size_t at = put_frame(two, CABLE_TYPE_JSON, 10, 1);
    const size_t first = at;
    at += put_frame(two + at, CABLE_TYPE_JSON, 10, 2);
    static cable_decoder_t fixed;
    static uint8_t buf[CABLE_MAX_FRAME];
    cable_decoder_buf_t d;
    reset_ctx_t r = { &fixed, 1, 0 };
    cable_decoder_init(&fixed);
    cable_decoder_feed(&fixed, two, at, reset_in_cb, &r);
    // Both frames decode: the reset drops only what was buffered, and nothing is lost when the call returns.
    CHECK(r.frames == 2 && fixed.len == 0);
    r = (reset_ctx_t){ &d, 0, 0 };
    CHECK(cable_decoder_init_with(&d, buf, sizeof buf));
    cable_decoder_buf_feed(&d, two, at, reset_in_cb, &r);
    CHECK(r.frames == 2 && d.len == 0);
    (void)first;

    // A callback reads the counters through the public struct: they are current when it runs, and a reset
    // inside it keeps them.
    static uint8_t noisy[16 + CABLE_MAX_FRAME];
    memset(noisy, 0x01, 5);
    const size_t m = 5 + put_frame(noisy + 5, CABLE_TYPE_JSON, 4, 7);
    r = (reset_ctx_t){ &fixed, 1, 0 };
    cable_decoder_init(&fixed);
    cable_decoder_feed(&fixed, noisy, m, count_cb, &r);
    CHECK(r.frames == 1 && fixed.discarded_bytes == 5 && fixed.len == 0);
}

// ── the vtable as a contract: a loopback transport and a wrapper over it ─────────────────────────────

typedef struct {
    uint8_t q[4 * CABLE_MAX_FRAME];   // bytes written by one side, read by the other
    size_t head, tail;
    int closed, opened, closes;
    size_t read_limit;                // a read returns at most this many bytes (split reads)
    size_t write_limit;               // a write accepts at most this many bytes (partial writes)
    int stall_writes;                 // the next N writes accept nothing
    int limited; size_t accept_left;  // limited: only accept_left more bytes will ever be taken
    int waits[16]; unsigned n_waits;  // the wait_ms values the reader passed
} loop_t;

static bool loop_open(const cable_xport_t *x) { loop_t *l = x->ctx; l->opened++; l->closed = 0; return true; }
static void loop_close(const cable_xport_t *x) { loop_t *l = x->ctx; l->closed = 1; l->closes++; }
static int loop_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    loop_t *l = x->ctx;
    if (l->n_waits < 16) l->waits[l->n_waits++] = wait_ms;
    if (l->closed) return -1;
    size_t n = l->tail - l->head;
    if (n > cap) n = cap;
    if (l->read_limit && n > l->read_limit) n = l->read_limit;
    memcpy(buf, l->q + l->head, n);
    l->head += n;
    return (int)n;                    // 0 when empty: nothing arrived in time
}
static int loop_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    loop_t *l = x->ctx;
    (void)wait_ms;
    if (l->closed) return -1;
    if (l->stall_writes > 0) { l->stall_writes--; return 0; }
    if (l->limited && l->accept_left == 0) return 0;
    size_t n = len;
    if (l->write_limit && n > l->write_limit) n = l->write_limit;
    if (l->limited && n > l->accept_left) n = l->accept_left;
    if (l->limited) l->accept_left -= n;
    CHECK(l->tail + n <= sizeof l->q);
    memcpy(l->q + l->tail, buf, n);
    l->tail += n;
    return (int)n;
}
static bool loop_is_up(const cable_xport_t *x) { loop_t *l = x->ctx; return l->opened && !l->closed; }

// The wrapper: bytes are XORed on the way through, standing in for the TLS record layer. Its write() takes
// the whole buffer or closes, which is the rule for a stream that cannot resync.
typedef struct { const cable_xport_t *self; unsigned retries_same_buffer; int gave_up; } wrap_t;
#define WRAP_KEY 0x5A
#define WRAP_BUDGET 64   // inner write attempts that return 0 before the wrapper gives up (stands in for the 2 s cap)

static bool wrap_open(const cable_xport_t *x) { return x->inner->open(x->inner); }
static void wrap_close(const cable_xport_t *x) { x->inner->close(x->inner); }
static int wrap_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    const int n = x->inner->read(x->inner, buf, cap, wait_ms);   // the BIO receive callback
    for (int i = 0; i < n; i++) buf[i] ^= WRAP_KEY;
    return n;
}
static int wrap_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    wrap_t *w = x->ctx;
    uint8_t enc[CABLE_MAX_FRAME];
    CHECK(len <= sizeof enc);
    for (size_t i = 0; i < len; i++) enc[i] = buf[i] ^ WRAP_KEY;
    size_t done = 0;
    unsigned stalls = 0;
    while (done < len) {
        const int n = x->inner->write(x->inner, enc + done, len - done, wait_ms);   // the BIO send callback
        if (n < 0) return -1;
        if (n == 0) {
            if (done == 0 && stalls == 0) { /* nothing taken yet: the caller may retry the same buffer */ }
            if (++stalls > WRAP_BUDGET) {
                w->gave_up++;
                if (done) { x->inner->close(x->inner); return -1; }   // half a record out: close, never resync
                return 0;
            }
            w->retries_same_buffer++;
            continue;
        }
        done += (size_t)n;
    }
    return (int)len;
}
static bool wrap_is_up(const cable_xport_t *x) { return x->inner->is_up(x->inner); }

static void test_wrapper_over_loopback(void)
{
    static loop_t loop;
    static wrap_t wrap;
    memset(&loop, 0, sizeof loop);
    const cable_xport_t inner = { "loop", CABLE_XPORT_BLE, loop_open, loop_close, loop_read, loop_write, loop_is_up, &loop, NULL, NULL };
    const cable_xport_t outer = { "xor", inner.kind, wrap_open, wrap_close, wrap_read, wrap_write, wrap_is_up, &wrap, &inner, NULL };
    wrap.self = &outer;

    CHECK(outer.kind == CABLE_XPORT_BLE && !strcmp(cable_xport_kind_name(outer.kind), "ble"));
    CHECK(!outer.is_up(&outer));
    CHECK(outer.open(&outer) && outer.is_up(&outer) && loop.opened == 1);

    uint8_t frame[CABLE_MAX_FRAME];
    const size_t n = put_frame(frame, CABLE_TYPE_JSON, 300, 9);

    // The inner stream takes 7 bytes at a time and stalls twice at the start; the wrapper keeps the same
    // buffer, finishes the call, and reports the whole length.
    loop.write_limit = 7;
    loop.stall_writes = 2;
    CHECK(outer.write(&outer, frame, n, 5) == (int)n);
    CHECK(wrap.retries_same_buffer == 2 && wrap.gave_up == 0);
    CHECK(loop.tail == n && memcmp(loop.q, frame, n) != 0);   // ciphertext on the inner wire

    // The reader gets it back one byte at a time and the decoder sees one frame.
    loop.read_limit = 1;
    cable_decoder_buf_t d;
    static uint8_t dbuf[CABLE_MAX_FRAME];
    CHECK(cable_decoder_init_with(&d, dbuf, sizeof dbuf));
    log_t l = {0};
    for (unsigned i = 0; i < n + 3; i++) {
        uint8_t b[16];
        const int got = outer.read(&outer, b, sizeof b, 100);
        CHECK(got == 0 || got == 1);
        if (got > 0) cable_decoder_buf_feed(&d, b, (size_t)got, collect, &l);
    }
    CHECK(l.n == 1 && l.s[0].len == 300 && l.s[0].first == 9);
    CHECK(loop.n_waits == 16 && loop.waits[0] == 100 && loop.waits[15] == 100);   // the wait budget reaches the inner read

    // A write that is never accepted returns 0 with nothing taken (retry-safe), and does not close.
    loop.stall_writes = 1000;
    wrap.gave_up = 0;
    CHECK(outer.write(&outer, frame, n, 5) == 0 && wrap.gave_up == 1 && !loop.closed);
    loop.stall_writes = 0;

    // The stream stalls for good after the first 10 bytes of a record: the wrapper has put half a record on
    // the wire, so it closes the inner stream and fails the write instead of returning a partial count.
    loop.write_limit = 10;
    loop.limited = 1; loop.accept_left = 10;
    wrap.gave_up = 0;
    CHECK(outer.write(&outer, frame, n, 5) < 0 && wrap.gave_up == 1 && loop.closed);
    loop.limited = 0;
    outer.close(&outer);
    CHECK(loop.closed && loop.closes == 2 && !outer.is_up(&outer));   // once by the failed write, once here
    uint8_t b[8];
    CHECK(outer.read(&outer, b, sizeof b, 0) < 0);
    CHECK(outer.write(&outer, frame, n, 5) < 0);
}

int main(void)
{
    test_init_with_rejects_bad_args();
    test_cap_1024();
    test_equivalent_to_fixed();
    test_reset_in_callback();
    test_wrapper_over_loopback();
    printf("cable xport: PASS (%u checks; decoder over a caller buffer incl. cap 1,024 reject and resync, "
           "equivalence with the fixed decoder, reset in callback, wrapper over a loopback transport)\n", checks);
    return 0;
}
