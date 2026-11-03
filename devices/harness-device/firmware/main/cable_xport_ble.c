#include "cable_xport_ble.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ble_xport";

#define MIN_SDU 23u          // the smallest SDU an L2CAP channel can have: used when the peer's MTU cannot be read
#define BUSY_SLICE_MS 10     // a BUSY answer is offered again after at most this long (or sooner, when the stack unstalls)
#define READ_SLICE_MS 100    // a long read wakes at least this often to look at the clock

static struct {
    cable_xport_ble_port_t port;
    bool                   inited;
    uint8_t               *ring;
    size_t                 head;           // index of the oldest byte
    size_t                 used;           // bytes held
    bool                   rearm_pending;  // an SDU arrived while no whole SDU fit: the stack has no receive buffer until the reader makes room
    SemaphoreHandle_t      lock;           // the ring and its flags
    SemaphoreHandle_t      data;           // binary: data arrived, or the channel changed state
    SemaphoreHandle_t      tx;             // binary: the stack unstalled the channel
    atomic_bool            up;
    atomic_bool            broken;         // the stream lost bytes: it gives nothing more
    atomic_bool            sending;        // one write() at a time
} S;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void lock(void) { xSemaphoreTake(S.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(S.lock); }

static void drain(SemaphoreHandle_t s) { while (xSemaphoreTake(s, 0) == pdTRUE) {} }

// ── the transport ───────────────────────────────────────────────────────────────────────────────────

static bool x_open(const cable_xport_t *x)
{
    (void)x;
    // ble_link opens the TLS wrapper over this only after the channel opened, so "open" is "the channel is up".
    return S.inited && atomic_load(&S.up);
}

static void x_close(const cable_xport_t *x)
{
    (void)x;
    if (!S.inited) return;
    if (atomic_exchange(&S.up, false)) S.port.disconnect(S.port.ctx);
    xSemaphoreGive(S.data);   // wake a reader and a writer: they see the channel is down
    xSemaphoreGive(S.tx);
}

static int x_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    if (!S.inited || !buf || cap == 0) return S.inited ? 0 : -1;
    const int64_t deadline = now_ms() + (wait_ms > 0 ? wait_ms : 0);
    for (;;) {
        bool rearm = false;
        size_t n = 0;
        lock();
        if (atomic_load(&S.broken)) {
            unlock();
            return -1;
        }
        n = S.used < cap ? S.used : cap;
        if (n) {
            const size_t first = CABLE_XPORT_BLE_RING - S.head < n ? CABLE_XPORT_BLE_RING - S.head : n;
            memcpy(buf, S.ring + S.head, first);
            if (n > first) memcpy(buf + first, S.ring, n - first);
            S.head = (S.head + n) % CABLE_XPORT_BLE_RING;
            S.used -= n;
            // The credit rule: the stack gets its next receive buffer only when a whole SDU fits.
            if (S.rearm_pending && CABLE_XPORT_BLE_RING - S.used >= CABLE_XPORT_BLE_SDU_MAX && atomic_load(&S.up)) {
                S.rearm_pending = false;
                rearm = true;
            }
        }
        unlock();
        if (n) {
            if (rearm) S.port.rearm(S.port.ctx);
            return (int)n;
        }
        if (!atomic_load(&S.up)) return -1;   // empty and closed
        const int64_t left = deadline - now_ms();
        if (wait_ms <= 0 || left <= 0) return 0;
        xSemaphoreTake(S.data, pdMS_TO_TICKS(left < READ_SLICE_MS ? left : READ_SLICE_MS));
    }
}

// The write failed after the stack took bytes, or the channel died: close it, and say so.
static int write_fail(const char *why)
{
    ESP_LOGW(TAG, "write: %s: closing the channel", why);
    x_close(NULL);
    atomic_store(&S.sending, false);
    return -1;
}

static int x_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)x;
    (void)wait_ms;   // no lock to wait for: one writer at a time, and a second one is told "0"
    if (!S.inited || !atomic_load(&S.up) || atomic_load(&S.broken)) return -1;
    if (!buf || len == 0) return 0;
    if (atomic_exchange(&S.sending, true)) return 0;
    const int64_t deadline = now_ms() + CABLE_XPORT_BLE_WRITE_CAP_MS;
    size_t taken = 0;
    while (taken < len) {
        if (!atomic_load(&S.up)) return write_fail("the channel closed");
        size_t mtu = S.port.peer_mtu(S.port.ctx);
        if (mtu < MIN_SDU) mtu = MIN_SDU;
        if (mtu > CABLE_XPORT_BLE_SDU_MAX) mtu = CABLE_XPORT_BLE_SDU_MAX;
        const size_t chunk = len - taken < mtu ? len - taken : mtu;
        drain(S.tx);   // an unstall from before this send must not release the stall this send may start
        const cable_xport_ble_send_t r = S.port.send(S.port.ctx, buf + taken, chunk);
        const int64_t left = deadline - now_ms();
        switch (r) {
        case BLE_SEND_OK:
            taken += chunk;
            break;
        case BLE_SEND_STALLED:
            // The stack took this SDU and has no credits for the next: wait for its unstall event (it may already have come).
            taken += chunk;
            if (left <= 0 || xSemaphoreTake(S.tx, pdMS_TO_TICKS(left)) != pdTRUE) {
                if (!atomic_load(&S.up)) return write_fail("the channel closed");
                return write_fail("no credits within the cap");
            }
            break;
        case BLE_SEND_BUSY:
            if (left <= 0) goto cap_expired;
            xSemaphoreTake(S.tx, pdMS_TO_TICKS(left < BUSY_SLICE_MS ? left : BUSY_SLICE_MS));
            break;
        case BLE_SEND_NOMEM:
            if (left <= 0) goto cap_expired;
            vTaskDelay(1);
            break;
        case BLE_SEND_ERR:
        default:
            return write_fail("the stack refused the SDU");
        }
        continue;
    cap_expired:
        // Nothing of this call was taken: the link is fine and the caller may retry with the same buffer. Something was: the stream cannot resync.
        if (taken == 0) {
            atomic_store(&S.sending, false);
            return 0;
        }
        return write_fail("stuck after taking part of a record");
    }
    atomic_store(&S.sending, false);
    return (int)len;
}

static bool x_is_up(const cable_xport_t *x)
{
    (void)x;
    return S.inited && atomic_load(&S.up);
}

static const cable_xport_t s_xport = {
    .name = "ble",
    .kind = CABLE_XPORT_BLE,
    .open = x_open,
    .close = x_close,
    .read = x_read,
    .write = x_write,
    .is_up = x_is_up,
};

const cable_xport_t *cable_xport_ble(void) { return &s_xport; }

// ── life cycle ──────────────────────────────────────────────────────────────────────────────────────

bool cable_xport_ble_init(const cable_xport_ble_port_t *port)
{
    if (!port || !port->send || !port->rearm || !port->disconnect || !port->peer_mtu) return false;
    if (S.inited) return true;
    memset(&S, 0, sizeof S);
    S.port = *port;
    // PSRAM first: the ring is written with the CPU only (never by DMA). Internal RAM is the fallback, not a reason to have no phone mode.
    S.ring = heap_caps_malloc(CABLE_XPORT_BLE_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!S.ring) S.ring = heap_caps_malloc(CABLE_XPORT_BLE_RING, MALLOC_CAP_8BIT);
    S.lock = xSemaphoreCreateMutex();
    S.data = xSemaphoreCreateBinary();
    S.tx = xSemaphoreCreateBinary();
    if (!S.ring || !S.lock || !S.data || !S.tx) {
        if (S.ring) heap_caps_free(S.ring);
        if (S.lock) vSemaphoreDelete(S.lock);
        if (S.data) vSemaphoreDelete(S.data);
        if (S.tx) vSemaphoreDelete(S.tx);
        memset(&S, 0, sizeof S);
        ESP_LOGE(TAG, "no memory for the BLE transport");
        return false;
    }
    atomic_init(&S.up, false);
    atomic_init(&S.broken, false);
    atomic_init(&S.sending, false);
    S.inited = true;
    return true;
}

void cable_xport_ble_deinit(void)
{
    if (!S.inited) return;
    atomic_store(&S.up, false);
    S.inited = false;
    heap_caps_free(S.ring);
    vSemaphoreDelete(S.lock);
    vSemaphoreDelete(S.data);
    vSemaphoreDelete(S.tx);
    memset(&S, 0, sizeof S);
}

void cable_xport_ble_channel_up(void)
{
    if (!S.inited) return;
    lock();
    S.head = S.used = 0;       // none of an earlier session reaches this one: its TLS state is gone
    S.rearm_pending = false;
    unlock();
    drain(S.data);
    drain(S.tx);
    atomic_store(&S.broken, false);
    atomic_store(&S.sending, false);
    atomic_store(&S.up, true);
}

void cable_xport_ble_channel_down(void)
{
    if (!S.inited) return;
    atomic_store(&S.up, false);
    xSemaphoreGive(S.data);
    xSemaphoreGive(S.tx);
}

bool cable_xport_ble_on_sdu(const uint8_t *data, size_t len)
{
    if (!S.inited || !atomic_load(&S.up) || atomic_load(&S.broken)) return false;
    if (len == 0) return true;
    bool rearm = false;
    lock();
    if (len > CABLE_XPORT_BLE_RING - S.used) {
        unlock();
        // A peer that sent past its credits: bytes are lost, the TLS stream cannot resync.
        atomic_store(&S.broken, true);
        ESP_LOGW(TAG, "an SDU of %u B did not fit: closing the channel", (unsigned)len);
        x_close(NULL);
        return false;
    }
    const size_t tail = (S.head + S.used) % CABLE_XPORT_BLE_RING;
    const size_t first = CABLE_XPORT_BLE_RING - tail < len ? CABLE_XPORT_BLE_RING - tail : len;
    memcpy(S.ring + tail, data, first);
    if (len > first) memcpy(S.ring, data + first, len - first);
    S.used += len;
    // The credit rule: re-arm only while a whole SDU still fits; otherwise the reader does it when it has made room.
    if (CABLE_XPORT_BLE_RING - S.used >= CABLE_XPORT_BLE_SDU_MAX) rearm = true;
    else S.rearm_pending = true;
    unlock();
    xSemaphoreGive(S.data);
    if (rearm) S.port.rearm(S.port.ctx);
    return true;
}

void cable_xport_ble_on_tx_unstalled(void)
{
    if (S.inited) xSemaphoreGive(S.tx);
}
