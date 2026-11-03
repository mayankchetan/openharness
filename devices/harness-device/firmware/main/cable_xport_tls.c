// TLS 1.2 PSK over any cable transport. The contract, the suites and the write rule are in cable_xport_tls.h.
//
// The inner transport is reached only through the two BIO callbacks below and only while `lock` is held, so the inner
// transport sees one caller at a time and one mbedtls_ssl_context is never in two tasks at once.
#include "cable_xport_tls.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

static const char *TAG = "tls";

#define TLS_HANDSHAKE_MS 5000   // from the moment the inner transport is up
#define TLS_INNER_UP_MS  5000   // how long open() waits for the inner transport to report up
#define TLS_WRITE_CAP_MS 2000   // once a record is composed: finish it or close (finding F3)
#define TLS_SLICE_MS     10     // longest a reader holds the mutex while it waits for data (inner transport without wait_readable, and the handshake)
#define TLS_WAIT_SLICE_MS 500   // longest one reader wait on the socket (no mutex held): bounds how late a reader notices a close
#define TLS_WAIT_SLICE_LONG_MS 2000   // ... for a reader that asked for at least this long (the relaxed read of S2, power_policy.h): each socket wait is a wake of the chip.
                                      // close() calls shutdown() first (cable_xport_tcp.c drop), which ends a select at once; this bound only matters where it does not
#define TLS_CLOSE_MS     100    // best effort close_notify

// Only TLS-ECDHE-PSK-AES-CBC suites. mbedtls_ssl_conf_ciphersuites keeps the pointer, so the list is static.
static const int s_suites[] = {
    MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256,   // 0xC037, the suite W0 negotiated
    MBEDTLS_TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA384,   // 0xC038
    0,
};

typedef struct {
    cable_xport_t        self;      // handed out; self.ctx points back at this struct
    const cable_xport_t *inner;
    SemaphoreHandle_t    lock;      // every ssl_read, ssl_write and handshake step, and every call into `inner`
    atomic_int           writers_waiting;
    atomic_bool          up;        // handshake finished and the link is intact
    atomic_bool          closing;   // close() is waiting for the mutex: long loops give up
    atomic_int           hs_error;  // the mbedTLS code that ended the last handshake (0: none); read by cable_xport_tls_last_error()
    // Below: only under `lock`.
    bool                 inner_closed;
    bool                 write_pending;   // a record is inside mbedTLS and one write() call is still finishing it
    unsigned             session;         // counts open() calls: a write that outlived its session must not continue in the next one
    bool                 ssl_ready;
    int64_t              deadline_us;   // 0 = none; the BIO times out once it has passed (handshake bound)
    mbedtls_ssl_config   conf;
    mbedtls_ssl_context  ssl;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} tls_t;

static int64_t now_us(void) { return esp_timer_get_time(); }

// ── mbedTLS BIO over the inner transport (cable_xport.h, "WRAPPING") ───────────────────────────────

static int bio_send(void *p, const unsigned char *buf, size_t len)
{
    tls_t *t = p;
    int n = t->inner->write(t->inner, buf, len, 0);
    if (n > 0) return n > (int)len ? (int)len : n;
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv_timeout(void *p, unsigned char *buf, size_t len, uint32_t ms)
{
    tls_t *t = p;
    if (t->deadline_us && now_us() >= t->deadline_us) return MBEDTLS_ERR_SSL_TIMEOUT;
    int n = t->inner->read(t->inner, buf, len, (int)ms);
    if (n > 0) return n > (int)len ? (int)len : n;
    if (n == 0) return ms ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_CONN_RESET;
}

static bool retryable(int r)
{
    return r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT;
}

// ── teardown ───────────────────────────────────────────────────────────────────────────────────────

// Under `lock`. Frees the per-connection state; the config (and the key copy in it) stays for the next open().
static void release_session(tls_t *t)
{
    if (t->ssl_ready) {
        mbedtls_ssl_free(&t->ssl);
        mbedtls_ctr_drbg_free(&t->drbg);
        mbedtls_entropy_free(&t->entropy);
        t->ssl_ready = false;
    }
}

static void close_inner(tls_t *t)
{
    if (!t->inner_closed) {
        t->inner_closed = true;
        t->inner->close(t->inner);
    }
}

// Under `lock`. The stream cannot resync, so a half-sent record or a dead peer ends the link: tell the inner transport.
static void kill_link(tls_t *t)
{
    atomic_store(&t->up, false);
    close_inner(t);
}

// ── open: inner open, then the handshake ───────────────────────────────────────────────────────────

static bool tls_handshake(tls_t *t)
{
    int64_t end = now_us() + (int64_t)TLS_HANDSHAKE_MS * 1000;
    t->deadline_us = end;
    int r;
    for (;;) {
        mbedtls_ssl_conf_read_timeout(&t->conf, TLS_SLICE_MS);
        r = mbedtls_ssl_handshake(&t->ssl);
        if (r == 0) break;
        if (!retryable(r)) break;
        if (atomic_load(&t->closing) || now_us() >= end) {
            r = MBEDTLS_ERR_SSL_TIMEOUT;
            break;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
    }
    t->deadline_us = 0;
    if (r != 0) {
        atomic_store(&t->hs_error, r);
        ESP_LOGW(TAG, "handshake failed: -0x%04X", (unsigned)-r);
    }
    return r == 0;
}

static bool tls_open(const cable_xport_t *x)
{
    tls_t *t = x->ctx;
    // USB stays plain by design, and the RNG precondition (cable_xport_tls.h) is about the radios: refuse before touching anything.
    if (t->inner->kind == CABLE_XPORT_USB) {
        ESP_LOGW(TAG, "refused: the USB transport is not wrapped in TLS");
        return false;
    }
    xSemaphoreTake(t->lock, portMAX_DELAY);
    bool ok = false;
    atomic_store(&t->up, false);
    atomic_store(&t->hs_error, 0);
    if (t->ssl_ready) {   // open() again without close(): free the old session and close the inner transport first
        release_session(t);
        close_inner(t);
    }
    t->inner_closed = false;
    t->write_pending = false;
    t->session++;
    t->deadline_us = 0;

    if (!t->inner->open(t->inner)) goto out;

    // The handshake starts only once the inner transport is up.
    int64_t up_end = now_us() + (int64_t)TLS_INNER_UP_MS * 1000;
    while (!t->inner->is_up(t->inner)) {
        if (now_us() >= up_end) {
            ESP_LOGW(TAG, "inner %s transport did not come up", t->inner->name);
            goto out;
        }
        vTaskDelay(1);
    }

    // Entropy is seeded per connection, right before the handshake: bring Wi-Fi or the BLE controller up first so the
    // ESP RNG is a true random source.
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_ssl_init(&t->ssl);
    t->ssl_ready = true;
    int r = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, NULL, 0);
    if (r != 0) {
        ESP_LOGW(TAG, "rng seed failed: -0x%04X", (unsigned)-r);
        goto out;
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (r != 0) {
        ESP_LOGW(TAG, "ssl setup failed: -0x%04X", (unsigned)-r);
        goto out;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, NULL, bio_recv_timeout);

    if (!tls_handshake(t)) goto out;
    atomic_store(&t->up, true);
    ok = true;

out:
    if (!ok) {
        release_session(t);
        close_inner(t);
    }
    xSemaphoreGive(t->lock);
    return ok;
}

// ── close ──────────────────────────────────────────────────────────────────────────────────────────

static void tls_close(const cable_xport_t *x)
{
    tls_t *t = x->ctx;
    atomic_store(&t->closing, true);   // a task inside a long wait notices and lets go of the mutex
    xSemaphoreTake(t->lock, portMAX_DELAY);
    if (t->ssl_ready && atomic_load(&t->up)) {
        // Best effort, one try: tell the peer the stream ended on purpose.
        t->deadline_us = now_us() + (int64_t)TLS_CLOSE_MS * 1000;
        (void)mbedtls_ssl_close_notify(&t->ssl);
        t->deadline_us = 0;
    }
    atomic_store(&t->up, false);
    release_session(t);
    close_inner(t);
    atomic_store(&t->closing, false);
    xSemaphoreGive(t->lock);
}

// ── read ───────────────────────────────────────────────────────────────────────────────────────────

static int64_t left_ms_up(int64_t end_us)
{
    int64_t left_us = end_us - now_us();
    return left_us <= 0 ? 0 : (left_us + 999) / 1000;
}

// A failed stream still waits out the rest of the reader's time (cable_xport.h), or the reader task would spin.
static int read_dead(int64_t end_us)
{
    int64_t left_ms = (end_us - now_us()) / 1000;
    if (left_ms > 0) vTaskDelay(pdMS_TO_TICKS(left_ms));
    return -1;
}

// How the reader waits (wake count is the point: a 2 s wait in S2 must not be 200 polls).
//   Inner transport with wait_readable (TCP): the reader waits for the socket WITHOUT the mutex, in waits of at most
//   TLS_WAIT_SLICE_MS, then takes the mutex and reads with no wait of its own. A writer finds the mutex free all the time.
//   The slice is a bound, not a poll rate: a close from another task ends the reader's wait through the socket where the
//   stack wakes a select on a closed socket, and within one slice where it does not (then `up` is seen false at the top).
//   Inner transport without it: the older way, polling in TLS_SLICE_MS slices with the mutex held per slice.
// Bytes mbedTLS already holds (the rest of a record the caller's buffer was too small for, or a record that arrived with the one
// before) never reach the socket again, so each call looks for them first, once, before it waits on the socket.
static int tls_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    tls_t *t = x->ctx;
    if (wait_ms < 0) wait_ms = 0;
    const int64_t end = now_us() + (int64_t)wait_ms * 1000;
    const bool can_wait = t->inner->wait_readable != NULL;
    bool socket_next = false;   // mbedTLS holds nothing more: the next step is to wait on the socket
    bool write_retry = false;   // the last read said WANT_WRITE: read again after a tick, with no wait on readability
    bool pace = false;          // the last socket wait failed, or said readable and the read still found nothing: one tick before the next pass, or a persistent fault would spin
    for (;;) {
        if (!atomic_load(&t->up) || atomic_load(&t->closing)) return read_dead(end);
        int64_t left_ms = left_ms_up(end);   // rounded up: a last fraction of a millisecond is one more wait, not a spin of zero waits
        if (can_wait && socket_next) {
            const int cap = wait_ms >= TLS_WAIT_SLICE_LONG_MS ? TLS_WAIT_SLICE_LONG_MS : TLS_WAIT_SLICE_MS;
            int w = left_ms > cap ? cap : (int)left_ms;
            // No mutex here. > 0: readable, or the end of the stream, which the read below reports. < 0: the inner transport
            // failed; the read below finds the same error and ends the link under the mutex. 0: timeout, look at the state again.
            const int ready = t->inner->wait_readable(t->inner, w);
            if (ready == 0) {
                if (now_us() >= end) return 0;
                continue;
            }
            pace = true;   // the wait did not time out: if the read below finds nothing, wait one tick before the next pass
            left_ms = left_ms_up(end);
        }
        if (xSemaphoreTake(t->lock, pdMS_TO_TICKS(left_ms)) == pdTRUE) {
            if (!atomic_load(&t->up) || !t->ssl_ready) {
                xSemaphoreGive(t->lock);
                return read_dead(end);
            }
            int64_t slice = 0;   // with a wait op the socket is readable (or mbedTLS holds bytes): read, do not wait
            if (can_wait && !socket_next && !write_retry && mbedtls_ssl_get_bytes_avail(&t->ssl) == 0 && mbedtls_ssl_check_pending(&t->ssl) == 0) {
                xSemaphoreGive(t->lock);
                socket_next = true;
                continue;
            }
            if (!can_wait) {
                slice = (end - now_us()) / 1000;
                if (slice < 0) slice = 0;
                if (slice > TLS_SLICE_MS) slice = TLS_SLICE_MS;
            }
            write_retry = false;
            mbedtls_ssl_conf_read_timeout(&t->conf, (uint32_t)slice);
            int r = mbedtls_ssl_read(&t->ssl, buf, cap);
            if (r > 0) {
                xSemaphoreGive(t->lock);
                return r;
            }
            if (r != 0 && !retryable(r)) {
                // close_notify from the peer, a reset, a bad record: the stream is over.
                if (r != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) ESP_LOGW(TAG, "read failed: -0x%04X", (unsigned)-r);
                kill_link(t);
                xSemaphoreGive(t->lock);
                return read_dead(end);
            }
            xSemaphoreGive(t->lock);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                // mbedTLS has to send before it can read on: waiting for the socket to become readable would not help (and with data there
                // would return at once, every time). Give the send a tick and read again.
                write_retry = true;
                socket_next = false;
                pace = false;
                vTaskDelay(1);
            } else {
                socket_next = true;   // mbedTLS took all it could from what was there and needs more from the socket
                if (pace) vTaskDelay(1);
                pace = false;
            }
        } else if (now_us() < end) {
            vTaskDelay(1);   // the mutex was busy and time remains: do not spin
        }
        if (now_us() >= end) return 0;
        // A writer is queued for the mutex: let it have it before this reader takes it again.
        while (atomic_load(&t->writers_waiting) > 0 && now_us() < end) vTaskDelay(1);
    }
}

// ── write ──────────────────────────────────────────────────────────────────────────────────────────

static int tls_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    tls_t *t = x->ctx;
    if (len == 0) return 0;
    if (!atomic_load(&t->up)) return -1;
    if (wait_ms < 0) wait_ms = 0;

    atomic_fetch_add(&t->writers_waiting, 1);
    BaseType_t got = xSemaphoreTake(t->lock, pdMS_TO_TICKS(wait_ms));
    atomic_fetch_sub(&t->writers_waiting, 1);
    if (got != pdTRUE) return atomic_load(&t->up) ? 0 : -1;   // nothing taken; the link is fine
    if (!atomic_load(&t->up) || !t->ssl_ready) {
        xSemaphoreGive(t->lock);
        return -1;
    }
    if (t->write_pending) {   // another write is mid-record; this one took nothing
        xSemaphoreGive(t->lock);
        return 0;
    }

    // From the first ssl_write call the record is inside mbedTLS and only the same arguments will finish it.
    const int64_t cap_us = now_us() + (int64_t)TLS_WRITE_CAP_MS * 1000;
    const unsigned session = t->session;
    size_t done = 0;
    int result = (int)len;
    while (done < len) {
        int r = mbedtls_ssl_write(&t->ssl, buf + done, len - done);
        if (r > 0) {
            done += (size_t)r;   // a frame over one record payload comes back in pieces
            t->write_pending = false;
            continue;
        }
        if (retryable(r) && !atomic_load(&t->closing) && now_us() < cap_us) {
            // The record is inside mbedTLS and only this buffer finishes it. Give up the mutex while waiting for room, so a
            // stalled send never stops the receive side (the peer may be waiting for us to read before it reads), and refuse
            // other writers until it is out (write_pending).
            t->write_pending = true;
            xSemaphoreGive(t->lock);
            vTaskDelay(1);
            atomic_fetch_add(&t->writers_waiting, 1);
            xSemaphoreTake(t->lock, portMAX_DELAY);
            atomic_fetch_sub(&t->writers_waiting, 1);
            if (!atomic_load(&t->up) || !t->ssl_ready || t->session != session) {   // the link ended while the mutex was free
                if (t->session == session) t->write_pending = false;   // in a newer session the flag belongs to its writer
                xSemaphoreGive(t->lock);
                return -1;
            }
            continue;   // same buffer, same length
        }
        ESP_LOGW(TAG, "write %s: -0x%04X", retryable(r) ? "stalled, link closed" : "failed", (unsigned)-r);
        result = -1;
        break;
    }
    t->write_pending = false;
    if (result < 0) kill_link(t);
    xSemaphoreGive(t->lock);
    return result;
}

static bool tls_is_up(const cable_xport_t *x)
{
    tls_t *t = x->ctx;
    return atomic_load(&t->up) && t->inner->is_up(t->inner);
}

// ── construction ───────────────────────────────────────────────────────────────────────────────────

const cable_xport_t *cable_xport_tls_wrap(const cable_xport_t *inner, const char *identity,
                                          const uint8_t *psk, size_t psk_len)
{
    if (!inner || !identity || !*identity || !psk || psk_len == 0 || psk_len > CABLE_XPORT_TLS_PSK_MAX) return NULL;
    // Internal RAM: the atomics and the mutex word must not sit in PSRAM.
    tls_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!t) return NULL;
    t->lock = xSemaphoreCreateMutex();
    if (!t->lock) {
        free(t);
        return NULL;
    }
    t->inner = inner;
    atomic_init(&t->writers_waiting, 0);
    atomic_init(&t->up, false);
    atomic_init(&t->closing, false);
    atomic_init(&t->hs_error, 0);

    mbedtls_ssl_config_init(&t->conf);
    int r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        // TLS 1.2 only: both ends of the range are pinned, whatever the build enables.
        mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
        // No session tickets: a resumed session would skip the PSK exchange. The product build has them compiled out
        // (CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS=n); this keeps any build that has them from offering one.
        mbedtls_ssl_conf_session_tickets(&t->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
        mbedtls_ssl_conf_ciphersuites(&t->conf, s_suites);
        // mbedTLS copies the key and the identity.
        r = mbedtls_ssl_conf_psk(&t->conf, psk, psk_len, (const unsigned char *)identity, strlen(identity));
    }
    if (r != 0) {
        ESP_LOGW(TAG, "config failed: -0x%04X", (unsigned)-r);
        mbedtls_ssl_config_free(&t->conf);
        vSemaphoreDelete(t->lock);
        free(t);
        return NULL;
    }

    t->self = (cable_xport_t){
        .name = "tls",
        .kind = inner->kind,
        .open = tls_open,
        .close = tls_close,
        .read = tls_read,
        .write = tls_write,
        .is_up = tls_is_up,
        .ctx = t,
        .inner = inner,
    };
    return &t->self;
}

int cable_xport_tls_last_error(const cable_xport_t *x)
{
    return x && x->ctx ? atomic_load(&((tls_t *)x->ctx)->hs_error) : 0;
}

void cable_xport_tls_free(const cable_xport_t *x)
{
    if (!x) return;
    tls_t *t = x->ctx;
    tls_close(x);
    mbedtls_ssl_config_free(&t->conf);   // zeroizes the key copy
    vSemaphoreDelete(t->lock);
    free(t);
}
