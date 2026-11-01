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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_hs_class.h"
#include "net_state.h"
#include "power_sleep.h"
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
static net_refusal_t s_refusals;           // handshakes in a row that the daemon answered with an alert (net_link task only)

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
        net_refusal_reset(&s_refusals);
        post(NET_EV_TCP_FAIL);
        return;
    }
    // Only a TLS alert from the daemon is a refusal (net_hs_class.h), and AUTH_FAILED needs three in a row: a reset, an EOF, a close_notify or a
    // timeout is an ordinary failed handshake on the 1..60 s ladder.
    const int err = cable_xport_tls_last_error(tls);
    const bool alert = net_hs_refused(err);
    const net_event_t ev = net_handshake_event(&s_refusals, alert);
    if (alert) {
        ESP_LOGW(TAG, "handshake answered with an alert (mbedTLS -0x%04X), %u in a row", (unsigned)-err, (unsigned)s_refusals.refusals);
    } else {
        ESP_LOGW(TAG, "handshake failed without an alert (mbedTLS -0x%04X)", (unsigned)-err);
    }
    post(ev);
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
        // POWER (W13). Idle, this task holds nothing: not in the backoff waits, not in the serve loop. Over the join and the handshake it holds
        // PP_HOLD_LINK, the power policy's hold for a transport with traffic, so the chip does not light-sleep in the middle of a handshake.
        // Released on the one line below on every path, before any wait. With battery sleep off these two calls are empty inline functions.
        power_sleep_hold(PP_HOLD_LINK);
        const bool joined = (nwifi_bits() & NWIFI_GOT_IP) || join_wifi();
        const int64_t t0_us = esp_timer_get_time();
        const bool opened = joined && tls->open(tls);   // TCP connect, then the handshake, both on this task
        power_sleep_release(PP_HOLD_LINK);
        if (!joined) {
            backoff(&ladder, "wifi");
            continue;
        }
        if (!opened) {
            note_open_failure(tls);
            backoff(&ladder, "daemon");
            continue;
        }
        net_refusal_reset(&s_refusals);
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

// Hand-off between net_link_start() (the caller, app_main) and net_init_task. Two parties touch `b`, so it carries a count: each calls
// boot_release() when it is done with `b`, and the one that brings the count to zero frees it. Nobody frees `b` otherwise, nobody notifies a task
// handle (the caller may be gone), and the caller waits on a semaphore that lives in `b` itself.
//
//   phase  WAITING    the caller is waiting for net_init
//          COMMITTED  net_init passed its point of no return (the caller can no longer give up, and net_init will start net_link)
//          ABANDONED  the caller timed out first; net_init finds this at the same point and undoes everything it made
typedef struct {
    config_net_t cfg;
    SemaphoreHandle_t done;     // given once by net_init when it has set `ok`
    atomic_int phase;
    atomic_int refs;
    bool ok;
} boot_t;
enum { PHASE_WAITING = 0, PHASE_COMMITTED = 1, PHASE_ABANDONED = 2 };

static void boot_release(boot_t *b)
{
    if (atomic_fetch_sub(&b->refs, 1) != 1) return;   // the other party still holds it
    zero(&b->cfg, sizeof b->cfg);
    if (b->done) vSemaphoreDelete(b->done);
    free(b);
}

// Take down whatever net_init made, in the order it made it: the link state, the transport the reader would use, the radio, the TLS wrapper.
// Only valid while no net_link task exists and cable_link has not been given the transport.
static void undo(const cable_xport_t *tls, bool radio)
{
    post(NET_EV_STOP);
    atomic_store(&s_tls, NULL);
    if (radio) nwifi_stop();
    if (tls) cable_xport_tls_free(tls);
}

// The internal-stack half of start-up (see net_link.h). Ends by deleting itself.
static void net_init_task(void *arg)
{
    boot_t *b = arg;
    const cable_xport_t *tls = NULL;
    bool radio = false;
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
        tls = cable_xport_tls_wrap(cable_xport_tcp(), identity, b->cfg.key, CFG_NET_KEY_LEN);   // copies the key
        zero(b->cfg.key, sizeof b->cfg.key);
        if (!tls) {
            ESP_LOGE(TAG, "no memory for the TLS wrapper");
            break;
        }
        if (!nwifi_start(b->cfg.ssid, b->cfg.pass, post)) break;   // copies the password into the driver, and takes the radio down again on failure
        radio = true;
        zero(b->cfg.pass, sizeof b->cfg.pass);
        int expected = PHASE_WAITING;
        if (!atomic_compare_exchange_strong(&b->phase, &expected, PHASE_COMMITTED)) {
            ESP_LOGW(TAG, "start-up was given up on while the radio came up: stopping it");   // the caller is gone and is in USB mode
            break;
        }
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
            ESP_LOGE(TAG, "net_link task create failed: taking the radio down, USB mode");
            break;
        }
        b->ok = true;
    } while (0);
    if (!b->ok) undo(tls, radio);   // the caller (or nobody, if it gave up) falls back to USB mode; nothing of Wi-Fi mode is left running
    zero(&b->cfg, sizeof b->cfg);
    ESP_LOGI(TAG, "net_init done ok=%d stack free_min=%u B", (int)b->ok, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(b->done);   // the caller may be gone; `done` lives in `b`, which this task still holds
    boot_release(b);
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
    b->done = xSemaphoreCreateBinary();
    atomic_init(&b->phase, PHASE_WAITING);
    atomic_init(&b->refs, 2);   // this caller and net_init_task
    if (!b->done) {
        ESP_LOGE(TAG, "no memory to start Wi-Fi mode: USB mode");
        atomic_store(&b->refs, 1);
        boot_release(b);
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi mode (wl_mode = 1): starting the radio");
    // Internal stack, on purpose: esp_wifi_start may write the PHY calibration to NVS (wifi_sta.h).
    if (xTaskCreate(net_init_task, "net_init", NET_INIT_STACK, b, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "net_init task create failed: USB mode");
        atomic_store(&b->refs, 1);   // no second party exists
        boot_release(b);
        return false;
    }
    bool finished = xSemaphoreTake(b->done, pdMS_TO_TICKS(20000)) == pdTRUE;
    if (!finished) {
        int expected = PHASE_WAITING;
        if (atomic_compare_exchange_strong(&b->phase, &expected, PHASE_ABANDONED)) {
            // net_init is still bringing the radio up. From here `b` is net_init's: it sees ABANDONED at its point of no return, takes the radio and
            // the TLS wrapper down again, and frees `b` (the count reaches zero when it releases). We never touch `b` again.
            ESP_LOGE(TAG, "Wi-Fi mode did not start in time: USB mode (net_init will stop the radio)");
            boot_release(b);
            return false;
        }
        // net_init had already passed its point of no return: all that is left for it is creating a task, so waiting for it is bounded.
        finished = xSemaphoreTake(b->done, portMAX_DELAY) == pdTRUE;
    }
    const bool ok = finished && b->ok;
    boot_release(b);
    if (!ok) ESP_LOGE(TAG, "Wi-Fi mode did not start: USB mode");
    return ok;
}
