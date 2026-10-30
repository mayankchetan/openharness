#include "net_link.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cable_client.h"
#include "cable_link.h"
#include "cable_xport_tcp.h"
#include "cable_xport_tls.h"
#include "config_store.h"
#include "device_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_hs_class.h"
#include "net_state.h"
#include "ram_telemetry.h"
#include "task_util.h"
#include "ui/ui_screens.h"
#include "wifi_sta.h"

static const char *TAG = "net";

#define NET_INIT_STACK 5120    // internal; esp_wifi_init + esp_wifi_start with the PHY calibration (high-water mark logged)
#define NET_LINK_STACK 8192    // PSRAM; the size the bench ran a TLS handshake on (high-water mark logged at every link up and down)
#define JOIN_WAIT_MS 15000     // association plus DHCP, per attempt
#define SLICE_MS 250           // longest a wait goes without letting the screen know the state changed
#define SERVE_MS 50            // the serve loop's tick: log drain, link and Wi-Fi checks
#define DRAIN_LINES 8          // log lines sent per tick
#define STALE_SESSION_MS 30000 // a link with no `welcome` for this long is dropped and rebuilt (a daemon that died without a reset)

// ── state ───────────────────────────────────────────────────────────────────────────────────────────

static atomic_int s_state = NET_OFF;       // net_state_t; changed only through post()
static atomic_bool s_ui_dirty;
static _Atomic(const cable_xport_t *) s_tls;
static unsigned s_links;                   // links established since boot (net_link task only)

// The only writer of s_state. Safe from the event task and from net_link: a compare-and-swap loop over the pure table.
static void post(net_event_t event)
{
    int old = atomic_load(&s_state);
    for (;;) {
        const net_state_t next = net_state_next((net_state_t)old, event);
        if (next == (net_state_t)old) return;
        if (atomic_compare_exchange_weak(&s_state, &old, next)) {
            ESP_LOGI(TAG, "state %s -> %s", net_state_name((net_state_t)old), net_state_name(next));
            atomic_store(&s_ui_dirty, true);
            return;
        }
    }
}

const char *net_link_status_text(void) { return net_state_caption((net_state_t)atomic_load(&s_state)); }

// Tell the screen when the caption changed. net_link calls this between its waits; the event task only sets the flag.
static void ui_flush(void)
{
    if (atomic_exchange(&s_ui_dirty, false)) ui_net_status_changed();
}

static void sleep_ms(uint32_t ms)
{
    while (ms) {
        const uint32_t slice = ms < SLICE_MS ? ms : SLICE_MS;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
        ui_flush();
    }
}

static void backoff(net_backoff_t *b, const char *what)
{
    const bool refused = atomic_load(&s_state) == NET_AUTH_FAILED;
    const unsigned attempt = b->attempt;
    const uint32_t ms = net_backoff_next_ms(b, refused, esp_random());
    ESP_LOGI(TAG, "retry in %u ms (attempt %u, %s, state %s)", (unsigned)ms, attempt + 1, what, net_state_name((net_state_t)atomic_load(&s_state)));
    sleep_ms(ms);
}

// ── the transport cable_link sees ───────────────────────────────────────────────────────────────────

// open() does nothing: net_link owns the connection, and cable_link must be able to start before the first handshake. A read on a link
// that is not up waits out its time and says -1 (cable_xport.h), which the reader treats as "nothing yet".
static bool x_open(const cable_xport_t *x) { (void)x; return true; }
static void x_close(const cable_xport_t *x) { (void)x; }   // cable_link closes only after a failed start; the connection is net_link's

static int x_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    const cable_xport_t *t = atomic_load(&s_tls);
    if (!t) {
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
    .name = "wifi",
    .kind = CABLE_XPORT_WIFI,
    .open = x_open,
    .close = x_close,
    .read = x_read,
    .write = x_write,
    .is_up = x_is_up,
};

const cable_xport_t *net_link_xport(void) { return &s_xport; }

// ── the loop ────────────────────────────────────────────────────────────────────────────────────────

// Associate and get an address. True with an address; false after a failure, with the state machine already told.
static bool join_wifi(void)
{
    // Already joined and waiting for DHCP (a previous pass timed out): do not call connect again, that would start the join over.
    if (!(nwifi_bits() & NWIFI_ASSOCIATED)) nwifi_connect();
    const int64_t start_us = esp_timer_get_time();
    uint32_t bits;
    do {
        bits = nwifi_wait(NWIFI_GOT_IP | NWIFI_DISCONNECTED, SLICE_MS);
        ui_flush();
    } while (!(bits & (NWIFI_GOT_IP | NWIFI_DISCONNECTED)) && (esp_timer_get_time() - start_us) / 1000 < JOIN_WAIT_MS);
    if (bits & NWIFI_GOT_IP) {
        nwifi_log_ap();
        return true;
    }
    if (bits & NWIFI_DISCONNECTED) return false;   // the event moved the state already
    if (bits & NWIFI_ASSOCIATED) {
        post(NET_EV_IP_TIMEOUT);   // joined, but DHCP has not answered: it may yet; the next pass finds the address if it did
    } else {
        nwifi_disconnect();     // still searching: stop and start over after the backoff
    }
    return false;
}

// open() failed: say why, in terms of the state machine. Codes only in the log, never a key or a name.
static void note_open_failure(const cable_xport_t *tls)
{
    if (!(nwifi_bits() & NWIFI_GOT_IP)) return;   // the access point went away under us; its own events say so
    if (!cable_xport_tcp_connected_once()) {
        ESP_LOGW(TAG, "daemon unreachable (errno %d)", cable_xport_tcp_last_errno());
        post(NET_EV_TCP_FAIL);
        return;
    }
    const int err = cable_xport_tls_last_error(tls);
    if (net_hs_refused(err)) {
        ESP_LOGW(TAG, "handshake refused by the daemon (mbedTLS -0x%04X)", (unsigned)-err);
        post(NET_EV_TLS_REFUSED);
    } else {
        ESP_LOGW(TAG, "handshake failed (mbedTLS -0x%04X)", (unsigned)-err);
        post(NET_EV_TLS_FAIL);
    }
}

static void log_stacks(const char *when)
{
    TaskHandle_t reader = xTaskGetHandle("cable_link");
    ESP_LOGI(TAG, "stacks %s: net_link free_min=%u B cable_link free_min=%d B", when, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             reader ? (int)uxTaskGetStackHighWaterMark(reader) : -1);
}

// Hold the link: send queued log lines, watch the stream, the access point and the session. Returns why it ended.
static const char *serve(const cable_xport_t *tls)
{
    int64_t stale_since_us = 0;
    for (;;) {
        const uint32_t bits = nwifi_wait(NWIFI_DISCONNECTED, SERVE_MS);
        if (!tls->is_up(tls)) return "link closed";
        if (bits & NWIFI_DISCONNECTED) return "wifi lost";
        if (!(bits & NWIFI_GOT_IP)) return "ip lost";
        cable_link_drain_logs(DRAIN_LINES);
        const int64_t now_us = esp_timer_get_time();
        if (cable_client_is_connected()) {
            stale_since_us = 0;
        } else if (!stale_since_us) {
            stale_since_us = now_us;
        } else if ((now_us - stale_since_us) / 1000 > STALE_SESSION_MS) {
            return "no session";
        }
        ui_flush();
    }
}

static void net_link_task(void *arg)
{
    (void)arg;
    const cable_xport_t *tls = atomic_load(&s_tls);
    net_backoff_t ladder;
    net_backoff_reset(&ladder);
    for (;;) {
        ui_flush();
        if (!(nwifi_bits() & NWIFI_GOT_IP) && !join_wifi()) {
            backoff(&ladder, "wifi");
            continue;
        }
        const int64_t t0_us = esp_timer_get_time();
        if (!tls->open(tls)) {   // TCP connect, then the handshake, both on this task
            note_open_failure(tls);
            backoff(&ladder, "daemon");
            continue;
        }
        post(NET_EV_LINK_UP);
        s_links++;
        ESP_LOGI(TAG, "link up #%u handshake_ms=%d boot_ms=%d", s_links, (int)((esp_timer_get_time() - t0_us) / 1000),
                 (int)(esp_timer_get_time() / 1000));
        log_stacks("at link up");
        ram_telemetry_checkpoint("net_link_up");
        const int64_t up_us = esp_timer_get_time();
        const char *why = serve(tls);
        const uint32_t up_ms = (uint32_t)((esp_timer_get_time() - up_us) / 1000);
        tls->close(tls);
        post(NET_EV_LINK_DOWN);
        ESP_LOGI(TAG, "link down after %u ms (%s)", (unsigned)up_ms, why);
        log_stacks("at link down");
        if (net_backoff_link_earned_reset(up_ms)) net_backoff_reset(&ladder);
        backoff(&ladder, "relink");
    }
}

// ── start-up ────────────────────────────────────────────────────────────────────────────────────────

static void zero(void *p, size_t n)
{
    volatile uint8_t *v = p;   // volatile so the compiler cannot drop the stores as dead
    while (n--) *v++ = 0;
}

typedef struct {
    config_net_t cfg;
    TaskHandle_t waiter;
    bool ok;
} boot_t;

// The internal-stack half of start-up (see net_link.h). Ends by deleting itself.
static void net_init_task(void *arg)
{
    boot_t *b = arg;
    b->ok = false;
    do {
        if (!cable_xport_tcp_set_peer(b->cfg.host, b->cfg.port)) {
            ESP_LOGE(TAG, "the stored daemon address is not usable");
            break;
        }
        char identity[DEVICE_MAC_STR_LEN];
        if (!device_mac_str(identity, sizeof identity)) {
            ESP_LOGE(TAG, "no MAC for the TLS identity");
            break;
        }
        const cable_xport_t *tls = cable_xport_tls_wrap(cable_xport_tcp(), identity, b->cfg.key, CFG_NET_KEY_LEN);   // copies the key
        if (!tls) {
            ESP_LOGE(TAG, "no memory for the TLS wrapper");
            break;
        }
        zero(b->cfg.key, sizeof b->cfg.key);
        if (!nwifi_start(b->cfg.ssid, b->cfg.pass, post)) {   // copies the password into the driver and scrubs its own copy
            cable_xport_tls_free(tls);
            break;
        }
        zero(b->cfg.pass, sizeof b->cfg.pass);
        atomic_store(&s_tls, tls);
        post(NET_EV_START);
        ram_telemetry_checkpoint("wifi_started");
        // net_link: PSRAM stack, because its call graph (net_link.h) holds no flash operation; falls back to internal by itself if PSRAM is full.
#if TASK_STACK_PSRAM_NET
        const BaseType_t task_ok = task_create_psram(net_link_task, "net_link", NET_LINK_STACK, NULL, 4, NULL, tskNO_AFFINITY);
#else
        const BaseType_t task_ok = xTaskCreate(net_link_task, "net_link", NET_LINK_STACK, NULL, 4, NULL);
#endif
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "net_link task create failed");
            post(NET_EV_STOP);   // the radio stays up with nobody driving it; the caller falls back to USB, and the next boot starts clean
            break;
        }
        b->ok = true;
    } while (0);
    zero(&b->cfg, sizeof b->cfg);
    ESP_LOGI(TAG, "net_init done ok=%d stack free_min=%u B", (int)b->ok, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    TaskHandle_t waiter = b->waiter;
    xTaskNotifyGive(waiter);   // b stays valid until the waiter has read b->ok
    vTaskDelete(NULL);
}

bool net_link_start(void)
{
    config_net_t cfg;   // app_main's stack, about 150 B; zeroed before this returns
    if (!config_net_load(&cfg) || cfg.mode != 1) {
        zero(&cfg, sizeof cfg);
        ESP_LOGI(TAG, "USB mode (Wi-Fi mode is off, or the Wi-Fi settings are not whole)");
        return false;
    }
    boot_t *b = heap_caps_calloc(1, sizeof *b, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b) {
        zero(&cfg, sizeof cfg);
        ESP_LOGE(TAG, "no memory to start Wi-Fi mode: USB mode");
        return false;
    }
    b->cfg = cfg;
    zero(&cfg, sizeof cfg);
    b->waiter = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Wi-Fi mode (wl_mode = 1): starting the radio");
    // Internal stack, on purpose: esp_wifi_start may write the PHY calibration to NVS (wifi_sta.h).
    if (xTaskCreate(net_init_task, "net_init", NET_INIT_STACK, b, 5, NULL) != pdPASS) {
        zero(b, sizeof *b);
        free(b);
        ESP_LOGE(TAG, "net_init task create failed: USB mode");
        return false;
    }
    const bool finished = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20000)) > 0;
    const bool ok = finished && b->ok;
    if (finished) free(b);   // a task that never finished still owns it
    if (!ok) ESP_LOGE(TAG, "Wi-Fi mode did not start: USB mode");
    return ok;
}
