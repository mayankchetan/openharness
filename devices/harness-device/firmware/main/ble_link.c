#include "ble_link.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ble_state.h"
#include "cable_client.h"
#include "cable_link.h"
#include "cable_xport_ble.h"
#include "cable_xport_tls.h"
#include "config_store.h"
#include "device_mac.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
#include "host/util/util.h"
#include "net_hs_class.h"
#include "net_state.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "power_sleep.h"
#include "ram_telemetry.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "task_util.h"
#include "ui/display.h"

static const char *TAG = "ble_link";

#define BLE_INIT_STACK 5120     // internal; nimble_port_init brings the controller up, which may read the PHY data from NVS
#define BLE_LINK_STACK 8192     // PSRAM; the size the TLS handshake ran on over Wi-Fi (high-water mark logged at every link up and down)
#define COC_PSM 0x0080          // spec: psm
#define COC_MTU CABLE_XPORT_BLE_SDU_MAX
#define SDU_BLK 512             // msys block size: the L2CAP MPS is 512 - 8 = 504 (spec: coc_mps_bytes)
#define RX_BLKS 24              // SDU receive buffers, in PSRAM (bench: a 4,032 B SDU takes 8 blocks)
#define TX_BLKS 16              // SDU transmit buffers, in PSRAM
#define TICK_MS 50              // the link task's tick: deadlines, advertising, serve
#define DRAIN_LINES 8           // log lines sent per tick
#define DLE_DELAY_MS 1000       // spec: dle_request_delay_ms
#define DLE_OCTETS 251          // spec: dle_octets
#define DLE_TIME_US 2120        // what the Pixel gave with 251 octets (bench)
#define INIT_WAIT_MS 20000      // how long ble_link_start waits for the host to sync
#define SYNC_WAIT_MS 8000

#ifdef BLE_DEV_ALLOW_UNBONDED
#define DEV_UNBONDED true
#else
#define DEV_UNBONDED false
#endif

void ble_store_config_init(void);   // the NVS-backed bond store of the IDF's nimble host (no header declares it)

// ── state ───────────────────────────────────────────────────────────────────────────────────────────

static ble_t s_ble;
static SemaphoreHandle_t s_lock;               // s_ble and the flags below
static bool s_started;
static atomic_bool s_abandoned;                // ble_link_start gave up while ble_init was still running
static _Atomic(const cable_xport_t *) s_tls;
static char s_mac[DEVICE_MAC_STR_LEN];
static uint8_t s_own_addr_type;
static SemaphoreHandle_t s_synced;             // given by the host's sync callback

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static struct ble_l2cap_chan *volatile s_chan;
static volatile bool s_adv_active;
static uint64_t s_adv_not_before_ms;           // a refused link waits this long before it advertises again (net_state.h refused ladder)
static uint64_t s_conn_ms;                     // uptime at the connect (the DLE request counts from here)
static bool s_dle_done;
static bool s_handshaking;                     // the link task is inside tls->open(): a channel close is posted after the verdict, not before
static bool s_close_pending;
static net_backoff_t s_refused_ladder;
static unsigned s_links;

static os_membuf_t *s_rx_mem, *s_tx_mem;
static struct os_mempool s_rx_mp, s_tx_mp;
static struct os_mbuf_pool s_rx_pool, s_tx_pool;
static uint8_t *s_sdu_buf;                     // one SDU copied out of its mbuf chain (PSRAM)

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void state_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void state_unlock(void) { xSemaphoreGive(s_lock); }

// The only writer of s_ble. Safe from the host task and from the link task.
static void post(ble_event_t event)
{
    state_lock();
    const ble_state_t before = s_ble.state;
    const bool changed = ble_post(&s_ble, event, now_ms());
    const ble_state_t after = s_ble.state;
    state_unlock();
    if (changed) ESP_LOGI(TAG, "state %s -> %s", ble_state_name(before), ble_state_name(after));
}

static ble_state_t current(uint64_t *since)
{
    state_lock();
    const ble_state_t s = s_ble.state;
    if (since) *since = s_ble.since_ms;
    state_unlock();
    return s;
}

ble_state_t ble_link_state(void)
{
    if (!s_started || !s_lock) return BLE_ST_OFF;
    return current(NULL);
}

bool ble_link_restart(void)
{
    if (!s_started) return false;
    post(BLE_EV_START);
    return true;
}

// ── SDU buffers ─────────────────────────────────────────────────────────────────────────────────────

static bool pools_alloc(void)
{
    if (!s_rx_mem) s_rx_mem = heap_caps_malloc(OS_MEMPOOL_SIZE(RX_BLKS, SDU_BLK) * sizeof(os_membuf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_mem) s_tx_mem = heap_caps_malloc(OS_MEMPOOL_SIZE(TX_BLKS, SDU_BLK) * sizeof(os_membuf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sdu_buf) s_sdu_buf = heap_caps_malloc(COC_MTU, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_rx_mem && s_tx_mem && s_sdu_buf;
}

static void pools_init(void)   // after nimble_port_init (the mempool registry exists)
{
    os_mempool_init(&s_rx_mp, RX_BLKS, SDU_BLK, s_rx_mem, "ble_rx");
    os_mbuf_pool_init(&s_rx_pool, &s_rx_mp, SDU_BLK, RX_BLKS);
    os_mempool_init(&s_tx_mp, TX_BLKS, SDU_BLK, s_tx_mem, "ble_tx");
    os_mbuf_pool_init(&s_tx_pool, &s_tx_mp, SDU_BLK, TX_BLKS);
}

static int rx_arm(struct ble_l2cap_chan *chan)
{
    struct os_mbuf *m = os_mbuf_get_pkthdr(&s_rx_pool, 0);
    if (!m) return BLE_HS_ENOMEM;
    return ble_l2cap_recv_ready(chan, m);
}

// ── the port of the transport (cable_xport_ble.h) ───────────────────────────────────────────────────

// Ownership of the SDU after ble_l2cap_send (read from ble_l2cap_coc.c of IDF 5.5.6 on the bench, task B6): BUSY and BADDATA come back before the stack takes
// it, so the caller frees; every other answer has already given the SDU to the stack, which frees it itself.
static cable_xport_ble_send_t port_send(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (!chan) return BLE_SEND_ERR;
    struct os_mbuf *m = os_mbuf_get_pkthdr(&s_tx_pool, 0);
    if (!m) return BLE_SEND_NOMEM;
    if (os_mbuf_append(m, data, len) != 0) {
        os_mbuf_free_chain(m);
        return BLE_SEND_NOMEM;
    }
    const int rc = ble_l2cap_send(chan, m);
    if (rc == 0) return BLE_SEND_OK;
    if (rc == BLE_HS_ESTALLED) return BLE_SEND_STALLED;
    if (rc == BLE_HS_EBUSY) {
        os_mbuf_free_chain(m);
        return BLE_SEND_BUSY;
    }
    if (rc == BLE_HS_EBADDATA) {
        os_mbuf_free_chain(m);
        return BLE_SEND_ERR;
    }
    return rc == BLE_HS_ENOMEM ? BLE_SEND_NOMEM : BLE_SEND_ERR;
}

static void port_disconnect(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (chan) ble_l2cap_disconnect(chan);
}

static void port_rearm(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    if (!chan) return;
    if (rx_arm(chan) != 0) {
        // No receive buffer for the next SDU: the phone would stall for ever. End the channel; the phone opens a new one.
        ESP_LOGW(TAG, "no receive buffer: closing the channel");
        ble_l2cap_disconnect(chan);
    }
}

static size_t port_peer_mtu(void *ctx)
{
    (void)ctx;
    struct ble_l2cap_chan *chan = s_chan;
    struct ble_l2cap_chan_info info;
    if (!chan || ble_l2cap_get_chan_info(chan, &info) != 0) return 0;
    return info.peer_coc_mtu;
}

static const cable_xport_ble_port_t PORT = {NULL, port_send, port_rearm, port_disconnect, port_peer_mtu};

// ── NimBLE events ───────────────────────────────────────────────────────────────────────────────────

static int coc_event(struct ble_l2cap_event *event, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);

static bool link_encrypted_and_bonded(uint16_t conn, bool *bonded)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) {
        *bonded = false;
        return false;
    }
    *bonded = d.sec_state.bonded;
    return d.sec_state.encrypted;
}

static void advertise(void)
{
    uint8_t adv[31], rsp[31];
    const size_t adv_n = ble_adv_data(s_mac, adv, sizeof adv), rsp_n = ble_scan_rsp_data(rsp, sizeof rsp);
    if (!adv_n || !rsp_n) {
        ESP_LOGE(TAG, "no advertising data");
        s_adv_not_before_ms = now_ms() + 1000;
        return;
    }
    struct ble_gap_adv_params ap;
    memset(&ap, 0, sizeof ap);
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min = ap.itvl_max = BLE_GAP_ADV_ITVL_MS(50);   // spec: adv_interval_ms
    int rc = ble_gap_adv_set_data(adv, (int)adv_n);
    if (rc == 0) rc = ble_gap_adv_rsp_set_data(rsp, (int)rsp_n);
    if (rc == 0) rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc == 0) {
        s_adv_active = true;
        char name[24];
        if (ble_adv_name(s_mac, name, sizeof name)) ESP_LOGI(TAG, "advertising name=%s", name);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "advertising did not start (rc=%d): trying again in a second", rc);
        s_adv_not_before_ms = now_ms() + 1000;
    } else {
        s_adv_active = true;
    }
}

static void terminate_link(void)
{
    const uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_active = false;   // a connection ends advertising either way
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_conn_ms = now_ms();
            s_dle_done = false;
            post(BLE_EV_LINK_UP);
            // Development build: no bond is asked for, so the link counts as encrypted from the start and the 3 s deadline never fires.
            if (!ble_encryption_required(DEV_UNBONDED)) post(BLE_EV_ENCRYPTED);
            ESP_LOGI(TAG, "central connected (conn %u)", (unsigned)s_conn);
        } else {
            ESP_LOGW(TAG, "connect failed (status %d)", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason 0x%x)", (unsigned)event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_adv_active = false;
        s_chan = NULL;
        cable_xport_ble_channel_down();
        post(BLE_EV_LINK_DOWN);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            bool bonded = false;
            const bool enc = link_encrypted_and_bonded(event->enc_change.conn_handle, &bonded);
            if (enc && (bonded || DEV_UNBONDED)) post(BLE_EV_ENCRYPTED);
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        return 0;
    default:
        return 0;   // no passkey UI yet (B5): a pairing that needs one fails, which is the safe answer
    }
}

static int coc_event(struct ble_l2cap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT: {
        bool bonded = false;
        const bool enc = link_encrypted_and_bonded(event->accept.conn_handle, &bonded);
        switch (ble_accept_check(enc, bonded, DEV_UNBONDED, s_chan != NULL)) {
        case BLE_ACCEPT_OK:
            return rx_arm(event->accept.chan);
        case BLE_ACCEPT_REFUSE_ENC:
            ESP_LOGW(TAG, "channel refused: link not encrypted and bonded");
            return BLE_HS_EENCRYPT;   // reaches the peer as "insufficient encryption" (ble_l2cap_sig.c)
        case BLE_ACCEPT_REFUSE_SECOND:
        default:
            ESP_LOGW(TAG, "channel refused: one is open already");
            return BLE_HS_ENOMEM;
        }
    }
    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status == 0) {
            s_chan = event->connect.chan;
            cable_xport_ble_channel_up();
            ESP_LOGI(TAG, "channel up psm=0x%04x mtu=%u", (unsigned)COC_PSM, (unsigned)port_peer_mtu(NULL));
            post(BLE_EV_CHANNEL_OPEN);
        } else {
            ESP_LOGW(TAG, "channel open failed (status %d)", event->connect.status);
        }
        return 0;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED: {
        s_chan = NULL;
        cable_xport_ble_channel_down();
        state_lock();
        const bool defer = s_handshaking;
        if (defer) s_close_pending = true;
        state_unlock();
        if (!defer) post(BLE_EV_CHANNEL_CLOSED);
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
        struct os_mbuf *sdu = event->receive.sdu_rx;
        const uint16_t len = OS_MBUF_PKTLEN(sdu);
        bool ok = len <= COC_MTU && os_mbuf_copydata(sdu, 0, len, s_sdu_buf) == 0;
        os_mbuf_free_chain(sdu);   // before the transport re-arms: the next receive buffer comes from the same pool
        if (!ok || !cable_xport_ble_on_sdu(s_sdu_buf, len)) {
            ESP_LOGW(TAG, "bad SDU (%u B): closing the channel", (unsigned)len);
            ble_l2cap_disconnect(event->receive.chan);
        }
        return 0;
    }
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        cable_xport_ble_on_tx_unstalled();
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset (reason %d)", reason); }

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &s_own_addr_type) == 0) {
        const int rc = ble_l2cap_create_server(COC_PSM, COC_MTU, coc_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "no L2CAP server (rc=%d)", rc);
        else xSemaphoreGive(s_synced);
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();   // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ── the link task ───────────────────────────────────────────────────────────────────────────────────

static void log_stacks(const char *when)
{
    TaskHandle_t reader = xTaskGetHandle("cable_link");
    ESP_LOGI(TAG, "stacks %s: ble_link free_min=%u B cable_link free_min=%d B", when, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             reader ? (int)uxTaskGetStackHighWaterMark(reader) : -1);
}

// The TLS handshake runs inside open() on this task, once the phone has opened the channel; 5 s at most (cable_xport_tls.c), and the channel is
// closed on any failure. A close that the host task reports meanwhile is held back until the verdict is in, so the refusal run sees the failure first.
static void handshake(const cable_xport_t *tls)
{
    state_lock();
    s_handshaking = true;
    s_close_pending = false;
    state_unlock();
    power_sleep_hold(PP_HOLD_LINK);   // the chip does not light-sleep in the middle of a handshake (empty inline with battery sleep off)
    const int64_t t0_us = esp_timer_get_time();
    const bool opened = tls->open(tls);
    power_sleep_release(PP_HOLD_LINK);
    const int ms = (int)((esp_timer_get_time() - t0_us) / 1000);
    if (opened) {
        post(BLE_EV_TLS_UP);
        s_links++;
        ESP_LOGI(TAG, "link up #%u handshake_ms=%d boot_ms=%d", s_links, ms, (int)(esp_timer_get_time() / 1000));
        log_stacks("at link up");
        ram_telemetry_checkpoint("ble_link_up");
    } else {
        // Only a TLS alert from the daemon is a refusal (net_hs_class.h); three in a row end the link. Anything else is an ordinary failure.
        const int err = cable_xport_tls_last_error(tls);
        const bool alert = net_hs_refused(err);
        state_lock();
        const ble_event_t verdict = ble_handshake_failed(&s_ble, alert, now_ms());
        const unsigned run = s_ble.refusals.refusals;
        state_unlock();
        if (alert) ESP_LOGW(TAG, "handshake answered with an alert (mbedTLS -0x%04X), %u in a row", (unsigned)-err, run);
        else ESP_LOGW(TAG, "handshake failed after %d ms without an alert (mbedTLS -0x%04X)", ms, (unsigned)-err);
        ESP_LOGI(TAG, "handshake verdict: %s", verdict == BLE_EV_TLS_REFUSED ? "key refused" : "failed");
    }
    state_lock();
    s_handshaking = false;
    const bool closed = s_close_pending;
    s_close_pending = false;
    state_unlock();
    if (closed) post(BLE_EV_CHANNEL_CLOSED);
}

static void ble_link_task(void *arg)
{
    (void)arg;
    const cable_xport_t *tls = atomic_load(&s_tls);
    net_backoff_reset(&s_refused_ladder);
    bool tls_open = false;     // the TLS session of the current channel is established and not yet closed
    bool refused_seen = false; // AUTH_FAILED already acted on
    uint64_t up_ms = 0;
    uint32_t seen_wake = display_wake_seq();   // the screen's wake counter: a touch or the PWR button moves it
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        const uint64_t now = now_ms();
        uint64_t since = 0;
        ble_state_t st = current(&since);

        // 1. deadlines
        state_lock();
        const ble_event_t due = ble_poll(&s_ble, now);
        state_unlock();
        if (due == BLE_EV_ADV_TIMEOUT) {
            ble_gap_adv_stop();
            s_adv_active = false;
            post(BLE_EV_ADV_TIMEOUT);
            ESP_LOGI(TAG, "no phone in %u ms: not found", (unsigned)BLE_ADV_TIMEOUT_MS);
        } else if (due == BLE_EV_ENC_TIMEOUT) {
            ESP_LOGW(TAG, "link not encrypted after %u ms: dropped", (unsigned)BLE_ENC_DEADLINE_MS);
            terminate_link();
            post(BLE_EV_ENC_TIMEOUT);
        } else if (due == BLE_EV_START) {
            post(BLE_EV_START);   // "Phone not found" has lasted BLE_RETRY_MS: advertise again (30 s on the air, 120 s off)
            ESP_LOGI(TAG, "no phone for %u ms: advertising again", (unsigned)BLE_RETRY_MS);
        } else if (due == BLE_EV_TLS_FAIL && !tls_open) {
            // open() has its own 5 s deadline, so this is a channel that opened and never got a handshake: close it, and count an ordinary failure
            port_disconnect(NULL);
            state_lock();
            (void)ble_handshake_failed(&s_ble, false, now);
            state_unlock();
        }
        st = current(&since);

        // 1b. a screen wake (a touch, the PWR button) while "not found": advertise again at once
        const uint32_t wake = display_wake_seq();
        if (wake != seen_wake) {
            seen_wake = wake;
            if (st == BLE_ST_NOT_FOUND) {
                post(BLE_EV_START);
                ESP_LOGI(TAG, "screen wake: advertising again");
                st = current(&since);
            }
        }

        // 2. advertising: while ADVERTISING and not on the air (the first time, after a disconnect, after a refused back-off)
        if (st == BLE_ST_ADVERTISING && !s_adv_active && now >= s_adv_not_before_ms) advertise();
        if (st != BLE_ST_ADVERTISING && s_adv_active && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_adv_stop();
            s_adv_active = false;
        }

        // 3. DLE one second after a connect. Never the 2M PHY (finding FB1): there is no PHY call anywhere in this file.
        const uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE && !s_dle_done && now - s_conn_ms >= DLE_DELAY_MS) {
            s_dle_done = true;
            const int rc = ble_gap_set_data_len(conn, DLE_OCTETS, DLE_TIME_US);
            ESP_LOGI(TAG, "data length request rc=%d", rc);
        }

        // 4. the TLS handshake when the channel has opened
        if (st == BLE_ST_CHANNEL_UP && !tls_open) {
            handshake(tls);
            st = current(&since);
            if (st == BLE_ST_TLS_UP) {
                tls_open = true;
                up_ms = now_ms();
                net_backoff_reset(&s_refused_ladder);
            }
        }

        // 5. the link is up: send queued log lines, notice the end
        if (st == BLE_ST_TLS_UP && tls_open) {
            cable_link_drain_logs(DRAIN_LINES);
            if (!tls->is_up(tls)) post(BLE_EV_CHANNEL_CLOSED);
        }
        if (tls_open && st != BLE_ST_TLS_UP) {
            tls->close(tls);   // the session is over: the TLS state goes, and the next channel starts a new handshake
            tls_open = false;
            ESP_LOGI(TAG, "link down after %u ms", (unsigned)(now_ms() - up_ms));
            log_stacks("at link down");
        }

        // 6. a refused key: drop the link, and wait out the refused back-off before advertising again
        if (st == BLE_ST_AUTH_FAILED && !refused_seen) {
            refused_seen = true;
            const uint32_t wait = net_backoff_next_ms(&s_refused_ladder, true, esp_random());
            s_adv_not_before_ms = now + wait;
            ESP_LOGW(TAG, "key refused: dropping the link, advertising again in %u ms", (unsigned)wait);
            terminate_link();
        }
        if (st != BLE_ST_AUTH_FAILED) refused_seen = false;
    }
}

// ── transport the cable layer sees ──────────────────────────────────────────────────────────────────

static bool x_open(const cable_xport_t *x) { (void)x; return true; }   // ble_link owns the connection; cable_link starts before the first channel
static void x_close(const cable_xport_t *x) { (void)x; }

static int x_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    if (!t || !t->is_up(t)) {
        if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return -1;
    }
    return t->read(t, buf, cap, wait_ms);
}

static int x_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    return t ? t->write(t, buf, len, wait_ms) : -1;
}

static bool x_is_up(const cable_xport_t *x)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    return t && t->is_up(t);
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

const cable_xport_t *ble_link_xport(void) { return &s_xport; }

// ── start-up ────────────────────────────────────────────────────────────────────────────────────────

static void zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

typedef struct {
    SemaphoreHandle_t done;
    bool ok;
} boot_t;

// The internal-stack half of start-up. Ends by deleting itself.
static void ble_init_task(void *arg)
{
    boot_t *b = arg;
    b->ok = false;
    do {
        if (!pools_alloc()) {
            ESP_LOGE(TAG, "no memory for the SDU buffers");
            break;
        }
        if (!cable_xport_ble_init(&PORT)) break;
        const esp_err_t e = nimble_port_init();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed (0x%x)", (unsigned)e);
            break;
        }
        ble_hs_cfg.reset_cb = on_reset;
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        // Pairing (B5 adds the screen): numeric comparison with a number on the dial, LE Secure Connections, bonds kept. With no passkey UI a pairing
        // that needs one fails, so a product image accepts only a phone that is already bonded.
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_svc_gap_init();
        ble_svc_gatt_init();
        char name[24];
        if (ble_adv_name(s_mac, name, sizeof name)) ble_svc_gap_device_name_set(name);
        ble_store_config_init();
        pools_init();
        nimble_port_freertos_init(host_task);
        if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(SYNC_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "the BLE host did not sync");
            break;
        }
        if (atomic_load(&s_abandoned)) {
            ESP_LOGW(TAG, "start-up was given up on: not starting the link");
            break;
        }
        ble_init(&s_ble);
        s_started = true;
        post(BLE_EV_START);
#if TASK_STACK_PSRAM_NET
        const BaseType_t task_ok = task_create_psram(ble_link_task, "ble_link", BLE_LINK_STACK, NULL, 4, NULL, tskNO_AFFINITY);
#else
        const BaseType_t task_ok = xTaskCreate(ble_link_task, "ble_link", BLE_LINK_STACK, NULL, 4, NULL);
#endif
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "ble_link task create failed: USB mode");
            s_started = false;
            break;
        }
        b->ok = true;
    } while (0);
    ESP_LOGI(TAG, "ble_init done ok=%d stack free_min=%u B", (int)b->ok, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(b->done);
    vTaskDelete(NULL);
}

bool ble_link_start(void)
{
    config_ble_t cfg;
    if (!config_ble_load(&cfg)) {
        zero(&cfg, sizeof cfg);
        ESP_LOGI(TAG, "no phone mode (the phone settings are not whole)");
        return false;
    }
    char identity[DEVICE_MAC_STR_LEN];
    if (!device_mac_str(identity, sizeof identity)) {
        zero(&cfg, sizeof cfg);
        ESP_LOGE(TAG, "no MAC for the TLS identity");
        return false;
    }
    memcpy(s_mac, identity, sizeof s_mac);
    const cable_xport_t *tls = NULL;
    s_lock = xSemaphoreCreateMutex();
    s_synced = xSemaphoreCreateBinary();
    if (s_lock && s_synced) tls = cable_xport_tls_wrap(cable_xport_ble(), identity, cfg.key, CFG_NET_KEY_LEN);   // copies the key
    zero(&cfg, sizeof cfg);
    if (!tls) {
        ESP_LOGE(TAG, "no memory for the TLS wrapper: USB mode");
        return false;
    }
    atomic_store(&s_tls, tls);
#ifdef BLE_DEV_ALLOW_UNBONDED
    ESP_LOGW(TAG, "DEVELOPMENT BUILD (BLE_DEV_ALLOW_UNBONDED): the phone is accepted without a bond; TLS-PSK is the only authentication");
#endif
    boot_t *b = calloc(1, sizeof *b);
    if (!b) {
        ESP_LOGE(TAG, "no memory to start the phone mode: USB mode");
        return false;
    }
    b->done = xSemaphoreCreateBinary();
    if (!b->done || xTaskCreate(ble_init_task, "ble_init", BLE_INIT_STACK, b, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ble_init task create failed: USB mode");
        if (b->done) vSemaphoreDelete(b->done);
        free(b);
        return false;
    }
    ESP_LOGI(TAG, "phone mode (wl_mode = 2): starting the BLE host");
    if (xSemaphoreTake(b->done, pdMS_TO_TICKS(INIT_WAIT_MS)) != pdTRUE) {
        // ble_init is still working. It sees the flag before it starts the link task and does not start it; `b` is leaked on purpose (it is still in use).
        atomic_store(&s_abandoned, true);
        ESP_LOGE(TAG, "the phone mode did not start in time: USB mode");
        return false;
    }
    const bool ok = b->ok;
    vSemaphoreDelete(b->done);
    free(b);
    if (!ok) ESP_LOGE(TAG, "the phone mode did not start: USB mode");
    return ok;
}
