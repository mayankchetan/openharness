// B2: cable_xport_tls.c against a real mbedTLS 3.6 (the IDF copy, built for the host) through a fake inner transport.
//
//   run by test_cable_xport_tls.py, which builds the mbedTLS sources under host_tls/mbedtls_host_config.h
//
// The peer is an mbedTLS PSK server on a thread. The wire between them is a pair of byte queues behind the
// cable_xport.h vtable, with knobs that return one byte at a time, accept one byte per write, stall, and close.
// Nothing in the wrapper is stubbed: the handshake, the record layer and the PSK exchange are mbedTLS's own.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
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

#include "mbedtls/aes.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "cable_frame.h"
#include "cable_xport_tls.h"

// ── bookkeeping ────────────────────────────────────────────────────────────────────────────────────

static atomic_uint checks;
static void dump_log(void);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); dump_log(); abort(); } atomic_fetch_add(&checks, 1); } while (0)

static int64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// every line the wrapper logs, for the "no key bytes" check
static pthread_mutex_t log_m = PTHREAD_MUTEX_INITIALIZER;
static char log_buf[1 << 16];
static size_t log_len;

void host_tls_log(char level, const char *tag, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    pthread_mutex_lock(&log_m);
    int w = snprintf(log_buf + log_len, sizeof(log_buf) - log_len, "%c %s: %s\n", level, tag, line);
    if (w > 0 && log_len + (size_t)w < sizeof(log_buf)) log_len += (size_t)w;
    pthread_mutex_unlock(&log_m);
}

static void dump_log(void)
{
    pthread_mutex_lock(&log_m);
    fprintf(stderr, "--- wrapper log ---\n%s", log_buf);
    pthread_mutex_unlock(&log_m);
}

static void log_clear(void) { pthread_mutex_lock(&log_m); log_len = 0; log_buf[0] = 0; pthread_mutex_unlock(&log_m); }

// ── ssl_write spy: the arguments of every call made by a client thread ───────────────────────────────

static _Thread_local int t_is_server;
typedef struct { const unsigned char *buf; size_t len; int ret; } write_call_t;
#define MAX_CALLS 200000
static write_call_t *calls;
static atomic_int ncalls;

int __real_mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len);
int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len)
{
    int r = __real_mbedtls_ssl_write(ssl, buf, len);
    if (!t_is_server) {
        int i = atomic_fetch_add(&ncalls, 1);
        if (calls && i < MAX_CALLS) calls[i] = (write_call_t){ buf, len, r };
    }
    return r;
}

// ── the wire: two byte queues ────────────────────────────────────────────────────────────────────────

#define QCAP (1 << 16)
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    uint8_t buf[QCAP];
    size_t head, len;
    bool closed;
} q_t;

static void q_init(q_t *q)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    pthread_mutex_init(&q->m, &ma);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q->c, &ca);
    q->head = q->len = 0;
    q->closed = false;
}

static void q_close(q_t *q)
{
    pthread_mutex_lock(&q->m);
    q->closed = true;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
}

// bytes taken (0 when full), or -1 when closed
static int q_write(q_t *q, const uint8_t *p, size_t n)
{
    pthread_mutex_lock(&q->m);
    int ret;
    if (q->closed) {
        ret = -1;
    } else {
        size_t take = QCAP - q->len;
        if (take > n) take = n;
        for (size_t i = 0; i < take; i++) q->buf[(q->head + q->len + i) % QCAP] = p[i];
        q->len += take;
        ret = (int)take;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ret;
}

// bytes read, 0 on timeout, -1 when closed and drained
static int q_read(q_t *q, uint8_t *p, size_t cap, int wait_ms, size_t max_per_call)
{
    pthread_mutex_lock(&q->m);
    if (q->len == 0 && !q->closed && wait_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += wait_ms / 1000;
        ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (q->len == 0 && !q->closed) {
            if (pthread_cond_timedwait(&q->c, &q->m, &ts) != 0) break;
        }
    }
    int ret;
    if (q->len == 0) {
        ret = q->closed ? -1 : 0;
    } else {
        size_t take = q->len < cap ? q->len : cap;
        if (max_per_call && take > max_per_call) take = max_per_call;
        for (size_t i = 0; i < take; i++) p[i] = q->buf[(q->head + i) % QCAP];
        q->head = (q->head + take) % QCAP;
        q->len -= take;
        ret = (int)take;
    }
    pthread_mutex_unlock(&q->m);
    return ret;
}

typedef struct {
    cable_xport_t x;         // first: the client's inner transport
    q_t *rx, *tx;
    atomic_int max_read;     // bytes per read call, 0 = as many as there are
    atomic_int max_write;    // bytes accepted per write call, 0 = all
    atomic_int stall_every;  // every Nth write call accepts nothing (0 = never)
    atomic_bool stall_all;   // every write call accepts nothing
    atomic_bool up;
    atomic_bool closed_by_wrapper;
    atomic_int write_calls;
    atomic_int open_calls;
} end_t;

static bool end_open(const cable_xport_t *x) { end_t *e = x->ctx; atomic_fetch_add(&e->open_calls, 1); atomic_store(&e->closed_by_wrapper, false); atomic_store(&e->up, true); return true; }

static void end_close(const cable_xport_t *x)
{
    end_t *e = x->ctx;
    atomic_store(&e->closed_by_wrapper, true);
    atomic_store(&e->up, false);
    q_close(e->rx);
    q_close(e->tx);
}

static int end_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    end_t *e = x->ctx;
    return q_read(e->rx, buf, cap, wait_ms, (size_t)atomic_load(&e->max_read));
}

static int end_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    end_t *e = x->ctx;
    (void)wait_ms;
    int n = atomic_fetch_add(&e->write_calls, 1) + 1;
    int every = atomic_load(&e->stall_every);
    if (atomic_load(&e->stall_all) || (every && n % every == 0)) {
        pthread_mutex_lock(&e->tx->m);
        bool closed = e->tx->closed;
        pthread_mutex_unlock(&e->tx->m);
        return closed ? -1 : 0;
    }
    int cap = atomic_load(&e->max_write);
    if (cap && len > (size_t)cap) len = (size_t)cap;
    return q_write(e->tx, buf, len);
}

static bool end_is_up(const cable_xport_t *x) { end_t *e = x->ctx; return atomic_load(&e->up); }

typedef struct { q_t c2s, s2c; end_t client; } link_t;

static void link_init(link_t *l, cable_xport_kind_t kind)
{
    q_init(&l->c2s);
    q_init(&l->s2c);
    end_t *e = &l->client;
    e->rx = &l->s2c;
    e->tx = &l->c2s;
    atomic_init(&e->max_read, 0);
    atomic_init(&e->max_write, 0);
    atomic_init(&e->stall_every, 0);
    atomic_init(&e->stall_all, false);
    atomic_init(&e->up, false);
    atomic_init(&e->closed_by_wrapper, false);
    atomic_init(&e->write_calls, 0);
    atomic_init(&e->open_calls, 0);
    e->x = (cable_xport_t){ .name = "fake", .kind = kind, .open = end_open, .close = end_close, .read = end_read,
                            .write = end_write, .is_up = end_is_up, .ctx = e };
}

static void link_reset(link_t *l)
{
    q_init(&l->c2s);
    q_init(&l->s2c);
    atomic_store(&l->client.stall_all, false);
    atomic_store(&l->client.stall_every, 0);
}

// ── the peer: an mbedTLS PSK server ──────────────────────────────────────────────────────────────────

enum { SRV_ECHO, SRV_SINK, SRV_SILENT, SRV_ECHO_THEN_NOTIFY, SRV_ECHO_THEN_DROP, SRV_DRIP_RECORD, SRV_PUSH };
static int make_frame(uint32_t seq, uint8_t *out, size_t cap);
#define PUSH_FRAMES 20

static const uint8_t KEY_A[32] = { 0x71, 0xa3, 0x0c, 0xd9, 0x42, 0xee, 0x15, 0x86, 0x3b, 0x90, 0x5f, 0xc7, 0x28, 0xb1, 0x6d, 0x04,
                                   0xe9, 0x33, 0xaa, 0x57, 0x1c, 0xf0, 0x8e, 0x62, 0xd5, 0x49, 0x0b, 0x7a, 0xc3, 0x98, 0x2f, 0x10 };
static const uint8_t KEY_B[32] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x0f, 0xed, 0xcb, 0xa9, 0x87, 0x65, 0x43, 0x21,
                                   0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90 };
#define IDENT "B8:1F:3F:CA:E5:58"

typedef struct {
    q_t *rx, *tx;
    int mode;
    const char *ident;          // the identity the server knows
    const uint8_t *key;         // and its key
    const int *suites;          // NULL = the two ECDHE-PSK CBC suites
    int byte_delay_us;          // throttle every byte it sends (slow-peer test)
    int echo_frames_before;     // for the *_THEN_* modes: stop after this many echoed reads
    pthread_t th;
    atomic_bool stop;
    atomic_bool finished;
    atomic_int hs_rc;
    char version[16];
    char suite[80];
    atomic_size_t rx_bytes;
} server_t;

static int srv_send(void *p, const unsigned char *buf, size_t len)
{
    server_t *s = p;
    size_t done = 0;
    while (done < len) {
        if (atomic_load(&s->stop)) return MBEDTLS_ERR_NET_SEND_FAILED;
        size_t chunk = s->byte_delay_us ? 1 : len - done;
        int n = q_write(s->tx, buf + done, chunk);
        if (n < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
        if (n == 0) { usleep(500); continue; }
        done += (size_t)n;
        if (s->byte_delay_us) usleep((useconds_t)s->byte_delay_us);
    }
    return (int)len;
}

static int srv_recv(void *p, unsigned char *buf, size_t len)
{
    server_t *s = p;
    for (;;) {
        if (atomic_load(&s->stop)) return MBEDTLS_ERR_NET_CONN_RESET;
        int n = q_read(s->rx, buf, len, 50, 0);
        if (n > 0) return n;
        if (n < 0) return MBEDTLS_ERR_NET_CONN_RESET;
    }
}

static int srv_psk_cb(void *p, mbedtls_ssl_context *ssl, const unsigned char *id, size_t id_len)
{
    server_t *s = p;
    if (id_len != strlen(s->ident) || memcmp(id, s->ident, id_len) != 0) return -1;
    return mbedtls_ssl_set_hs_psk(ssl, s->key, 32);
}

static const int DEFAULT_SUITES[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, MBEDTLS_TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA384, 0 };

static void *server_main(void *arg)
{
    server_t *s = arg;
    t_is_server = 1;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    if (s->mode == SRV_SILENT) {
        while (!atomic_load(&s->stop)) usleep(2000);
        goto done;
    }
    if (s->mode == SRV_DRIP_RECORD) {
        // A handshake record header that claims 16,000 bytes, then one byte every 8 ms: always inside the reader's 10 ms slice.
        static const uint8_t hdr[5] = { 22, 3, 3, 0x3E, 0x80 };
        uint8_t b = 0;
        if (q_write(s->tx, hdr, sizeof(hdr)) < 0) goto done;
        while (!atomic_load(&s->stop) && q_write(s->tx, &b, 1) >= 0) usleep(8000);
        goto done;
    }
    CHECK(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, NULL, 0) == 0);
    CHECK(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&conf, s->suites ? s->suites : DEFAULT_SUITES);
    mbedtls_ssl_conf_psk_cb(&conf, srv_psk_cb, s);
    CHECK(mbedtls_ssl_setup(&ssl, &conf) == 0);
    mbedtls_ssl_set_bio(&ssl, s, srv_send, srv_recv, NULL);

    int r = mbedtls_ssl_handshake(&ssl);
    atomic_store(&s->hs_rc, r);
    if (r != 0) goto done;
    snprintf(s->version, sizeof(s->version), "%s", mbedtls_ssl_get_version(&ssl));
    snprintf(s->suite, sizeof(s->suite), "%s", mbedtls_ssl_get_ciphersuite(&ssl));

    if (s->mode == SRV_PUSH) {   // say something unprompted, then swallow whatever arrives
        static _Thread_local uint8_t frame[CABLE_MAX_FRAME];
        for (uint32_t i = 0; i < PUSH_FRAMES; i++) {
            int n = make_frame(i, frame, sizeof(frame));
            for (int off = 0; off < n;) {
                int w = mbedtls_ssl_write(&ssl, frame + off, (size_t)(n - off));
                if (w <= 0) goto done;
                off += w;
            }
        }
    }
    static _Thread_local uint8_t buf[9000];
    int reads = 0;
    for (;;) {
        r = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
        if (r <= 0) break;
        atomic_fetch_add(&s->rx_bytes, (size_t)r);
        reads++;
        if (s->mode == SRV_SINK || s->mode == SRV_PUSH) continue;
        size_t off = 0;
        while (off < (size_t)r) {
            int w = mbedtls_ssl_write(&ssl, buf + off, (size_t)r - off);
            if (w <= 0) goto done;
            off += (size_t)w;
        }
        if (s->mode == SRV_ECHO_THEN_NOTIFY && reads >= s->echo_frames_before) {
            mbedtls_ssl_close_notify(&ssl);
            break;
        }
        if (s->mode == SRV_ECHO_THEN_DROP && reads >= s->echo_frames_before) break;
    }
done:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    q_close(s->tx);   // the client's read sees the end of the stream
    q_close(s->rx);
    atomic_store(&s->finished, true);
    return NULL;
}

static void server_start(server_t *s, link_t *l, int mode)
{
    memset(s, 0, sizeof(*s));
    s->rx = &l->c2s;
    s->tx = &l->s2c;
    s->mode = mode;
    s->ident = IDENT;
    s->key = KEY_A;
    atomic_init(&s->stop, false);
    atomic_init(&s->finished, false);
    atomic_init(&s->hs_rc, 1);
    atomic_init(&s->rx_bytes, 0);
}

static void server_run(server_t *s) { CHECK(pthread_create(&s->th, NULL, server_main, s) == 0); }

static void server_join(server_t *s)
{
    atomic_store(&s->stop, true);
    pthread_join(s->th, NULL);
}

// ── frames ─────────────────────────────────────────────────────────────────────────────────────────

typedef struct { uint32_t next; uint32_t bad; uint32_t seen; } check_t;

static size_t payload_len_for(uint32_t seq) { return 8 + (size_t)((seq * 2654435761u) % 3000u) + (seq % 17 == 0 ? 5000 : 0); }

static int make_frame(uint32_t seq, uint8_t *out, size_t cap)
{
    uint8_t payload[CABLE_MAX_PAYLOAD];
    size_t n = payload_len_for(seq);
    CHECK(n <= CABLE_MAX_PAYLOAD);
    memcpy(payload, &seq, 4);
    uint32_t x = seq * 747796405u + 2891336453u;
    for (size_t i = 4; i < n; i++) { x = x * 1664525u + 1013904223u; payload[i] = (uint8_t)(x >> 24); }
    return cable_frame_encode(CABLE_TYPE_LOG, payload, n, out, cap);
}

static void on_frame(uint8_t version, uint8_t type, const uint8_t *p, size_t n, void *ctx)
{
    check_t *c = ctx;
    (void)version;
    c->seen++;
    uint32_t seq;
    if (type != CABLE_TYPE_LOG || n < 4) { c->bad++; return; }
    memcpy(&seq, p, 4);
    if (seq != c->next || n != payload_len_for(seq)) { c->bad++; return; }
    uint32_t x = seq * 747796405u + 2891336453u;
    for (size_t i = 4; i < n; i++) { x = x * 1664525u + 1013904223u; if (p[i] != (uint8_t)(x >> 24)) { c->bad++; return; } }
    c->next++;
}

// write one frame whole; retries a 0 (mutex busy) with the same buffer
static int send_frame(const cable_xport_t *t, uint32_t seq)
{
    static _Thread_local uint8_t frame[CABLE_MAX_FRAME];
    int n = make_frame(seq, frame, sizeof(frame));
    CHECK(n > 0);
    for (int tries = 0; tries < 100; tries++) {
        int w = t->write(t, frame, (size_t)n, 100);
        if (w == n) return n;
        if (w < 0) return -1;
        CHECK(w == 0);
    }
    return 0;
}

// read until `want` frames decoded or `limit_ms` passes
static void read_frames(const cable_xport_t *t, cable_decoder_buf_t *d, check_t *c, uint32_t want, int limit_ms)
{
    int64_t end = ms_now() + limit_ms;
    uint8_t chunk[1500];
    while (c->seen < want) {
        // limit_ms == 0: take what is there now and return, so a sender can keep the echo from piling up
        int n = t->read(t, chunk, sizeof(chunk), limit_ms ? 50 : 0);
        if (n < 0) break;
        if (n > 0) cable_decoder_buf_feed(d, chunk, (size_t)n, on_frame, c);
        else if (!limit_ms || ms_now() >= end) break;
    }
}

static uint8_t dec_store[CABLE_MAX_FRAME];

static void decoder_init(cable_decoder_buf_t *d) { CHECK(cable_decoder_init_with(d, dec_store, sizeof(dec_store))); }

static const cable_xport_t *wrap(link_t *l, const uint8_t *key)
{
    const cable_xport_t *t = cable_xport_tls_wrap(&l->client.x, IDENT, key, 32);
    CHECK(t != NULL);
    return t;
}

// ── tests ──────────────────────────────────────────────────────────────────────────────────────────

static void test_wrap_arguments(void)
{
    link_t l;
    link_init(&l, CABLE_XPORT_BLE);
    CHECK(cable_xport_tls_wrap(NULL, IDENT, KEY_A, 32) == NULL);
    CHECK(cable_xport_tls_wrap(&l.client.x, NULL, KEY_A, 32) == NULL);
    CHECK(cable_xport_tls_wrap(&l.client.x, "", KEY_A, 32) == NULL);
    CHECK(cable_xport_tls_wrap(&l.client.x, IDENT, NULL, 32) == NULL);
    CHECK(cable_xport_tls_wrap(&l.client.x, IDENT, KEY_A, 0) == NULL);
    CHECK(cable_xport_tls_wrap(&l.client.x, IDENT, KEY_A, 33) == NULL);
    const cable_xport_t *t = wrap(&l, KEY_A);
    CHECK(strcmp(t->name, "tls") == 0);
    CHECK(t->kind == CABLE_XPORT_BLE);   // a wrapper reports the inner kind
    CHECK(t->inner == &l.client.x);
    CHECK(!t->is_up(t));
    uint8_t b[4] = { 0 };
    CHECK(t->write(t, b, 4, 10) < 0);    // not open
    CHECK(t->read(t, b, 4, 0) < 0);
    cable_xport_tls_free(t);
    cable_xport_tls_free(NULL);
    puts("ok wrap arguments");
}

// handshake and an echo of `frames` frames, with the inner transport throttled as asked
static void run_echo(const char *name, int max_read, int max_write, uint32_t frames)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    atomic_store(&l->client.max_read, max_read);
    atomic_store(&l->client.max_write, max_write);
    server_t s;
    server_start(&s, l, SRV_ECHO);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    int64_t t0 = ms_now();
    CHECK(t->open(t));
    int64_t hs_ms = ms_now() - t0;
    CHECK(t->is_up(t));
    CHECK(atomic_load(&l->client.open_calls) == 1);

    cable_decoder_buf_t d;
    decoder_init(&d);
    check_t c = { 0 };
    uint32_t sent = 0;
    while (sent < frames) {
        CHECK(send_frame(t, sent) > 0);
        sent++;
        // keep the echo flowing so the byte queues never fill
        read_frames(t, &d, &c, sent, 0);
    }
    read_frames(t, &d, &c, frames, 20000);
    CHECK(c.seen == frames);
    CHECK(c.bad == 0);
    CHECK(d.corrupt_frames == 0 && d.discarded_bytes == 0);
    CHECK(strcmp(s.version, "TLSv1.2") == 0);
    CHECK(strcmp(s.suite, "TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA256") == 0);   // 0xC037 first
    CHECK(atomic_load(&s.hs_rc) == 0);

    t->close(t);
    CHECK(atomic_load(&l->client.closed_by_wrapper));
    CHECK(!t->is_up(t));
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok %s: %u frames echoed intact, handshake %lld ms\n", name, frames, (long long)hs_ms);
}

static void test_echo(void)
{
    run_echo("echo, whole reads and writes", 0, 0, 200);
    run_echo("echo, one byte per inner read and write", 1, 1, 60);
    run_echo("echo, 7-byte reads, 13-byte writes", 7, 13, 60);
}

static void expect_refused(const char *name, const uint8_t *client_key, const char *srv_ident, const int *suites)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_ECHO);
    s.ident = srv_ident;
    s.suites = suites;
    server_run(&s);
    const cable_xport_t *t = wrap(l, client_key);
    log_clear();
    int64_t t0 = ms_now();
    CHECK(!t->open(t));
    int64_t ms = ms_now() - t0;
    CHECK(ms < 4500);
    CHECK(!t->is_up(t));
    CHECK(atomic_load(&l->client.closed_by_wrapper));   // the inner transport was closed again
    uint8_t b[8];
    CHECK(t->write(t, b, 8, 10) < 0);
    pthread_mutex_lock(&log_m);
    CHECK(strstr(log_buf, "handshake failed: -0x") != NULL);   // an error code, nothing else
    pthread_mutex_unlock(&log_m);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok %s: refused in %lld ms\n", name, (long long)ms);
}

static void test_refusals(void)
{
    expect_refused("wrong PSK", KEY_B, IDENT, NULL);
    expect_refused("unknown identity", KEY_A, "AA:BB:CC:DD:EE:FF", NULL);
    // The server below offers only suites the dial must never offer; it has every one of them built in.
    static const int plain_psk[] = { MBEDTLS_TLS_PSK_WITH_AES_128_CBC_SHA256, MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256, 0 };
    expect_refused("plain PSK suites only", KEY_A, IDENT, plain_psk);
    static const int sha1_suite[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA, MBEDTLS_TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA, 0 };
    expect_refused("ECDHE-PSK SHA-1 suites only", KEY_A, IDENT, sha1_suite);
    static const int dhe_psk[] = { MBEDTLS_TLS_DHE_PSK_WITH_AES_128_CBC_SHA256, 0 };
    expect_refused("DHE-PSK suite only", KEY_A, IDENT, dhe_psk);
}

static void test_second_suite(void)
{
    // A server that only knows 0xC038 still works: the second suite is on the list.
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    static const int only256[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA384, 0 };
    server_t s;
    server_start(&s, l, SRV_ECHO);
    s.suites = only256;
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    CHECK(send_frame(t, 1) > 0);
    cable_decoder_buf_t d;
    decoder_init(&d);
    check_t c = { .next = 1 };
    read_frames(t, &d, &c, 1, 5000);
    CHECK(c.seen == 1 && c.bad == 0);
    CHECK(strcmp(s.suite, "TLS-ECDHE-PSK-WITH-AES-256-CBC-SHA384") == 0);
    t->close(t);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    puts("ok second suite 0xC038 accepted");
}

static void test_handshake_deadline(void)
{
    // (a) a peer that never answers
    {
        link_t *l = calloc(1, sizeof(*l));
        link_init(l, CABLE_XPORT_WIFI);
        server_t s;
        server_start(&s, l, SRV_SILENT);
        server_run(&s);
        const cable_xport_t *t = wrap(l, KEY_A);
        int64_t t0 = ms_now();
        CHECK(!t->open(t));
        int64_t ms = ms_now() - t0;
        CHECK(ms >= 4900 && ms < 5800);
        CHECK(atomic_load(&l->client.closed_by_wrapper));
        server_join(&s);
        cable_xport_tls_free(t);
        free(l);
        printf("ok deadline, silent peer: open failed after %lld ms\n", (long long)ms);
    }
    // (b) a peer that drips a record faster than the read slice, so no single read ever times out
    {
        link_t *l = calloc(1, sizeof(*l));
        link_init(l, CABLE_XPORT_WIFI);
        server_t s;
        server_start(&s, l, SRV_DRIP_RECORD);
        server_run(&s);
        const cable_xport_t *t = wrap(l, KEY_A);
        int64_t t0 = ms_now();
        CHECK(!t->open(t));
        int64_t ms = ms_now() - t0;
        CHECK(ms >= 4900 && ms < 5800);
        server_join(&s);
        cable_xport_tls_free(t);
        free(l);
        printf("ok deadline, record dripped inside the read slice: open failed after %lld ms\n", (long long)ms);
    }
    // (c) a real server that drips its bytes slower than the deadline allows
    {
        link_t *l = calloc(1, sizeof(*l));
        link_init(l, CABLE_XPORT_WIFI);
        server_t s;
        server_start(&s, l, SRV_ECHO);
        s.byte_delay_us = 25000;   // 25 ms per byte: the server flight is a few hundred bytes
        server_run(&s);
        const cable_xport_t *t = wrap(l, KEY_A);
        int64_t t0 = ms_now();
        CHECK(!t->open(t));
        int64_t ms = ms_now() - t0;
        CHECK(ms >= 4900 && ms < 5800);
        server_join(&s);
        cable_xport_tls_free(t);
        free(l);
        printf("ok deadline, dripping peer: open failed after %lld ms\n", (long long)ms);
    }
}

// ── concurrency ────────────────────────────────────────────────────────────────────────────────────

typedef struct {
    const cable_xport_t *t;
    uint32_t frames;
    atomic_bool stop;
    int64_t max_write_ms;
    uint32_t seen, bad;
    _Atomic int rc;   // read by the main thread while the worker may still be running
} duo_t;

static void *duo_writer(void *p)
{
    duo_t *d = p;
    for (uint32_t i = 0; i < d->frames; i++) {
        int64_t t0 = ms_now();
        if (send_frame(d->t, i) < 0) { d->rc = -1; return NULL; }
        int64_t ms = ms_now() - t0;
        if (ms > d->max_write_ms) d->max_write_ms = ms;
    }
    return NULL;
}

static void *duo_reader(void *p)
{
    duo_t *d = p;
    cable_decoder_buf_t dec;
    uint8_t *store = malloc(CABLE_MAX_FRAME);
    CHECK(cable_decoder_init_with(&dec, store, CABLE_MAX_FRAME));
    check_t c = { 0 };
    uint8_t chunk[1500];
    int64_t last = ms_now();
    while (c.seen < d->frames && !atomic_load(&d->stop)) {
        int n = d->t->read(d->t, chunk, sizeof(chunk), 100);
        if (n < 0) { d->rc = -2; break; }
        if (n > 0) { cable_decoder_buf_feed(&dec, chunk, (size_t)n, on_frame, &c); last = ms_now(); }
        if (ms_now() - last > 15000) { d->rc = -3; break; }
    }
    d->seen = c.seen;
    d->bad = c.bad + dec.corrupt_frames + dec.discarded_bytes;
    free(store);
    return NULL;
}

static void test_concurrent_echo(void)
{
    for (int variant = 0; variant < 2; variant++) {
        link_t *l = calloc(1, sizeof(*l));
        link_init(l, CABLE_XPORT_WIFI);
        if (variant == 1) {   // the awkward wire: tiny reads, tiny writes, a stall every few calls
            atomic_store(&l->client.max_read, 3);
            atomic_store(&l->client.max_write, 29);
            atomic_store(&l->client.stall_every, 5);
        }
        server_t s;
        server_start(&s, l, SRV_ECHO);
        server_run(&s);
        const cable_xport_t *t = wrap(l, KEY_A);
        CHECK(t->open(t));
        duo_t d = { .t = t, .frames = variant ? 80 : 400 };
        atomic_init(&d.stop, false);
        pthread_t w, r;
        CHECK(pthread_create(&r, NULL, duo_reader, &d) == 0);
        CHECK(pthread_create(&w, NULL, duo_writer, &d) == 0);
        pthread_join(w, NULL);
        pthread_join(r, NULL);
        CHECK(d.rc == 0);
        CHECK(d.seen == d.frames);
        CHECK(d.bad == 0);
        t->close(t);
        server_join(&s);
        cable_xport_tls_free(t);
        free(l);
        printf("ok concurrent writer and reader%s: %u frames, none corrupt, max write %lld ms\n",
               variant ? " on the awkward wire" : "", d.frames, (long long)d.max_write_ms);
    }
}

static void test_writer_not_starved(void)
{
    // The server swallows everything and says nothing, so the reader thread waits for data the whole time.
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_SINK);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    duo_t d = { .t = t, .frames = 1000000 };   // the reader never gets there; it is stopped below
    atomic_init(&d.stop, false);
    pthread_t r;
    CHECK(pthread_create(&r, NULL, duo_reader, &d) == 0);
    usleep(50000);
    int64_t worst = 0, total = 0;
    const int N = 300;
    for (int i = 0; i < N; i++) {
        int64_t t0 = ms_now();
        CHECK(send_frame(t, (uint32_t)i) > 0);
        int64_t ms = ms_now() - t0;
        total += ms;
        if (ms > worst) worst = ms;
    }
    atomic_store(&d.stop, true);
    pthread_join(r, NULL);
    CHECK(d.rc == 0);
    CHECK(worst < 120);   // one 10 ms read slice, a yield tick, and the record itself
    t->close(t);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok writer not starved by a waiting reader: %d writes, worst %lld ms, mean %lld ms\n", N, (long long)worst, (long long)(total / N));
}

// ── partial write and stalls ───────────────────────────────────────────────────────────────────────

static void test_partial_write_same_buffer(void)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    atomic_store(&l->client.max_write, 100);
    atomic_store(&l->client.stall_every, 3);   // every third inner write takes nothing
    server_t s;
    server_start(&s, l, SRV_ECHO);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    atomic_store(&ncalls, 0);

    cable_decoder_buf_t d;
    decoder_init(&d);
    check_t c = { 0 };
    const uint32_t N = 20;
    for (uint32_t i = 0; i < N; i++) CHECK(send_frame(t, i) > 0);
    read_frames(t, &d, &c, N, 20000);
    CHECK(c.seen == N && c.bad == 0);

    int n = atomic_load(&ncalls);
    CHECK(n > 0 && n < MAX_CALLS);
    int want_write = 0, partial = 0;
    for (int i = 0; i + 1 < n; i++) {
        if (calls[i].ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            want_write++;
            // mbedTLS needs the same arguments after WANT_WRITE; the next call must be the retry, byte for byte
            CHECK(calls[i + 1].buf == calls[i].buf);
            CHECK(calls[i + 1].len == calls[i].len);
        }
        if (calls[i].ret > 0 && (size_t)calls[i].ret < calls[i].len) partial++;
    }
    CHECK(want_write > 20);   // the stalls did reach the record layer
    CHECK(partial > 0);       // frames over one record payload came back in pieces and were continued
    t->close(t);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok partial write keeps the same buffer: %d WANT_WRITE retries checked, %d split frames, %d ssl_write calls\n", want_write, partial, n);
}

static void test_write_stall_closes(void)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_SINK);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    CHECK(send_frame(t, 1) > 0);   // healthy first

    atomic_store(&l->client.stall_all, true);   // the far end stops accepting; the next record is composed and cannot leave
    uint8_t frame[CABLE_MAX_FRAME];
    int n = make_frame(2, frame, sizeof(frame));
    int64_t t0 = ms_now();
    int w = t->write(t, frame, (size_t)n, 50);
    int64_t ms = ms_now() - t0;
    CHECK(w < 0);
    CHECK(ms >= 1900 && ms < 2600);   // the 2 s cap, not the 50 ms wait
    CHECK(!t->is_up(t));
    CHECK(atomic_load(&l->client.closed_by_wrapper));
    CHECK(t->write(t, frame, (size_t)n, 50) < 0);
    int64_t t1 = ms_now();
    uint8_t b[16];
    CHECK(t->read(t, b, sizeof(b), 100) < 0);
    CHECK(ms_now() - t1 >= 90);   // a dead stream still waits out the reader's wait (cable_xport.h)
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok write that cannot finish: link closed after %lld ms (cap 2 s), later calls fail\n", (long long)ms);
}

static void *stalled_writer(void *p)
{
    duo_t *d = p;
    uint8_t frame[CABLE_MAX_FRAME];
    int n = make_frame(7, frame, sizeof(frame));
    d->rc = d->t->write(d->t, frame, (size_t)n, 5000);
    return NULL;
}

static void test_busy_mutex_returns_zero(void)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_SINK);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    atomic_store(&l->client.stall_all, true);
    duo_t d = { .t = t, .rc = 12345 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, stalled_writer, &d) == 0);
    usleep(300000);   // the first writer now holds the mutex, mid-record
    uint8_t one[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int64_t t0 = ms_now();
    int w = t->write(t, one, sizeof(one), 60);
    int64_t ms = ms_now() - t0;
    CHECK(w == 0);              // nothing taken: the first write is still finishing its record
    CHECK(ms < 250);
    CHECK(t->is_up(t));         // and the link is still up
    pthread_join(th, NULL);
    CHECK(d.rc < 0);            // the stalled writer gave up at its 2 s cap
    CHECK(!t->is_up(t));
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok second write while one is mid-record: returned 0 after %lld ms, link intact until the first gave up\n", (long long)ms);
}

static void test_reads_continue_during_write_stall(void)
{
    // A send that cannot finish must not stop the receive side: the peer may be waiting for us to read before it reads.
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_PUSH);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    atomic_store(&l->client.stall_all, true);
    duo_t d = { .t = t, .rc = 12345 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, stalled_writer, &d) == 0);
    cable_decoder_buf_t dec;
    decoder_init(&dec);
    check_t c = { 0 };
    int64_t t0 = ms_now();
    read_frames(t, &dec, &c, PUSH_FRAMES, 1500);
    int64_t ms = ms_now() - t0;
    CHECK(c.seen == PUSH_FRAMES && c.bad == 0);
    CHECK(ms < 1500);
    CHECK(d.rc == 12345);       // and the writer was still stuck while we read
    pthread_join(th, NULL);
    CHECK(d.rc < 0);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok reads continue while a send is stalled: %d frames read in %lld ms, then the send gave up\n", PUSH_FRAMES, (long long)ms);
}

// ── peer closes, reopen, poll ──────────────────────────────────────────────────────────────────────

static void test_peer_close(void)
{
    for (int mode = 0; mode < 2; mode++) {
        link_t *l = calloc(1, sizeof(*l));
        link_init(l, CABLE_XPORT_WIFI);
        server_t s;
        server_start(&s, l, mode ? SRV_ECHO_THEN_DROP : SRV_ECHO_THEN_NOTIFY);
        s.echo_frames_before = 1;
        server_run(&s);
        const cable_xport_t *t = wrap(l, KEY_A);
        CHECK(t->open(t));
        CHECK(send_frame(t, 1) > 0);
        cable_decoder_buf_t d;
        decoder_init(&d);
        check_t c = { .next = 1 };
        read_frames(t, &d, &c, 1, 5000);
        CHECK(c.seen == 1);
        int64_t t0 = ms_now();
        uint8_t b[64];
        int n = 0;
        for (int i = 0; i < 20 && n >= 0; i++) n = t->read(t, b, sizeof(b), 100);
        CHECK(n < 0);
        CHECK(!t->is_up(t));
        CHECK(ms_now() - t0 < 3000);
        server_join(&s);
        t->close(t);
        cable_xport_tls_free(t);
        free(l);
        printf("ok peer %s: read returns -1, is_up false\n", mode ? "drops the stream" : "sends close_notify");
    }
}

static void test_reopen(void)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    const cable_xport_t *t = wrap(l, KEY_A);
    for (int round = 0; round < 3; round++) {
        server_t s;
        server_start(&s, l, SRV_ECHO);
        server_run(&s);
        CHECK(t->open(t));
        CHECK(t->is_up(t));
        CHECK(send_frame(t, (uint32_t)round) > 0);
        cable_decoder_buf_t d;
        decoder_init(&d);
        check_t c = { .next = (uint32_t)round };
        read_frames(t, &d, &c, 1, 5000);
        CHECK(c.seen == 1 && c.bad == 0);
        t->close(t);
        CHECK(!t->is_up(t));
        server_join(&s);
        link_reset(l);
    }
    CHECK(atomic_load(&l->client.open_calls) == 3);
    cable_xport_tls_free(t);
    free(l);
    puts("ok close and open again, three sessions on one wrapper");
}

static void test_poll_and_idle(void)
{
    link_t *l = calloc(1, sizeof(*l));
    link_init(l, CABLE_XPORT_WIFI);
    server_t s;
    server_start(&s, l, SRV_SINK);
    server_run(&s);
    const cable_xport_t *t = wrap(l, KEY_A);
    CHECK(t->open(t));
    uint8_t b[32];
    int64_t t0 = ms_now();
    CHECK(t->read(t, b, sizeof(b), 0) == 0);
    int64_t poll_ms = ms_now() - t0;
    CHECK(poll_ms < 20);
    t0 = ms_now();
    CHECK(t->read(t, b, sizeof(b), 250) == 0);
    int64_t wait_ms = ms_now() - t0;
    CHECK(wait_ms >= 240 && wait_ms < 400);
    CHECK(t->is_up(t));
    t->close(t);
    server_join(&s);
    cable_xport_tls_free(t);
    free(l);
    printf("ok poll returns at once (%lld ms), a 250 ms wait takes %lld ms\n", (long long)poll_ms, (long long)wait_ms);
}

static void test_no_key_in_logs(void)
{
    // Every failure above logged an error code. Look for the key in what was logged, in both spellings.
    pthread_mutex_lock(&log_m);
    for (size_t off = 0; off + 8 <= 32; off += 4) {
        char hex[17], upper[17];
        for (int i = 0; i < 8; i++) { sprintf(hex + 2 * i, "%02x", KEY_A[off + i]); sprintf(upper + 2 * i, "%02X", KEY_A[off + i]); }
        CHECK(strstr(log_buf, hex) == NULL && strstr(log_buf, upper) == NULL);
        CHECK(memmem(log_buf, log_len, KEY_A + off, 8) == NULL);
        for (int i = 0; i < 8; i++) { sprintf(hex + 2 * i, "%02x", KEY_B[off + i]); sprintf(upper + 2 * i, "%02X", KEY_B[off + i]); }
        CHECK(strstr(log_buf, hex) == NULL && strstr(log_buf, upper) == NULL);
    }
    pthread_mutex_unlock(&log_m);
    puts("ok no key bytes in the wrapper's log output");
}

// ── connect mode: the wrapper over a real TCP socket, against another TLS-PSK server (the Node fixture) ──────────

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

typedef struct { cable_xport_t x; const char *host; int port; atomic_int fd; } tcp_t;

static bool tcp_open(const cable_xport_t *x)
{
    tcp_t *t = (tcp_t *)x;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)t->port) };
    if (fd < 0 || inet_pton(AF_INET, t->host, &a.sin_addr) != 1 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (fd >= 0) close(fd);
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    atomic_store(&t->fd, fd);
    return true;
}

static void tcp_close(const cable_xport_t *x)
{
    tcp_t *t = (tcp_t *)x;
    int fd = atomic_exchange(&t->fd, -1);
    if (fd >= 0) close(fd);
}

static int tcp_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    tcp_t *t = (tcp_t *)x;
    struct pollfd p = { .fd = atomic_load(&t->fd), .events = POLLIN };
    if (p.fd < 0) return -1;
    int pr = poll(&p, 1, wait_ms);
    if (pr == 0) return 0;
    if (pr < 0) return -1;
    ssize_t n = recv(p.fd, buf, cap, 0);
    return n > 0 ? (int)n : -1;   // 0 = orderly close
}

static int tcp_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)wait_ms;
    tcp_t *t = (tcp_t *)x;
    int fd = atomic_load(&t->fd);
    if (fd < 0) return -1;
    ssize_t n = send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

static bool tcp_is_up(const cable_xport_t *x) { return atomic_load(&((tcp_t *)x)->fd) >= 0; }

// connect HOST PORT IDENTITY KEYHEX FRAMES: handshake, then FRAMES frames out and echoed back intact. Exit 0 ok, 3 handshake refused, 4 data error.
static int run_connect(char **argv)
{
    tcp_t tcp = { .host = argv[0], .port = atoi(argv[1]) };
    atomic_init(&tcp.fd, -1);
    tcp.x = (cable_xport_t){ .name = "tcp", .kind = CABLE_XPORT_WIFI, .open = tcp_open, .close = tcp_close, .read = tcp_read,
                             .write = tcp_write, .is_up = tcp_is_up, .ctx = &tcp };
    uint8_t key[32];
    CHECK(strlen(argv[3]) == 64);
    for (int i = 0; i < 32; i++) { unsigned v; CHECK(sscanf(argv[3] + 2 * i, "%2x", &v) == 1); key[i] = (uint8_t)v; }
    uint32_t frames = (uint32_t)atoi(argv[4]);
    const cable_xport_t *t = cable_xport_tls_wrap(&tcp.x, argv[2], key, 32);
    CHECK(t != NULL);
    int64_t t0 = ms_now();
    if (!t->open(t)) {
        printf("REFUSED after %lld ms\n", (long long)(ms_now() - t0));
        cable_xport_tls_free(t);
        return 3;
    }
    printf("HANDSHAKE %lld ms\n", (long long)(ms_now() - t0));
    cable_decoder_buf_t d;
    decoder_init(&d);
    check_t c = { 0 };
    for (uint32_t i = 0; i < frames; i++) {
        if (send_frame(t, i) <= 0) { puts("WRITE FAILED"); cable_xport_tls_free(t); return 4; }
        read_frames(t, &d, &c, i + 1, 0);
    }
    read_frames(t, &d, &c, frames, 20000);
    t->close(t);
    cable_xport_tls_free(t);
    if (c.seen != frames || c.bad || d.corrupt_frames || d.discarded_bytes) {
        printf("DATA ERROR seen=%u bad=%u\n", c.seen, c.bad);
        return 4;
    }
    printf("ECHOED %u frames intact\n", frames);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 7 && strcmp(argv[1], "--connect") == 0) return run_connect(argv + 2);
    {   // mbedTLS builds its AES tables on first use without a lock (no MBEDTLS_THREADING in this config); do it once, before any thread
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        CHECK(mbedtls_aes_setkey_enc(&aes, KEY_A, 128) == 0);
        mbedtls_aes_free(&aes);
    }
    calls = calloc(MAX_CALLS, sizeof(*calls));
    CHECK(calls != NULL);
    log_clear();
    test_wrap_arguments();
    test_echo();
    test_second_suite();
    test_refusals();
    test_partial_write_same_buffer();
    test_concurrent_echo();
    test_writer_not_starved();
    test_write_stall_closes();
    test_busy_mutex_returns_zero();
    test_reads_continue_during_write_stall();
    test_peer_close();
    test_reopen();
    test_poll_and_idle();
    test_handshake_deadline();
    test_no_key_in_logs();
    printf("cable_xport_tls: %u checks PASS\n", checks);
    return 0;
}
