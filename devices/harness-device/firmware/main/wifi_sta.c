#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

// Not "wifi": that is the tag of the closed Wi-Fi driver, whose level nwifi_start() lowers (see LOG LEVELS in wifi_sta.h).
static const char *TAG = "nwifi";

static EventGroupHandle_t s_bits;
static nwifi_event_cb s_cb;
static esp_netif_t *s_netif;
static int64_t s_connect_us;   // when the last nwifi_connect() ran, for ms_to_ip

// Runs on the system event task (internal stack, 2,304 B): bits, one log line, one callback. Never the SSID, the password or the AP's address.
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            const wifi_event_sta_connected_t *e = data;
            xEventGroupClearBits(s_bits, NWIFI_DISCONNECTED);
            xEventGroupSetBits(s_bits, NWIFI_ASSOCIATED);
            ESP_LOGI(TAG, "associated channel=%u authmode=%d", (unsigned)e->channel, (int)e->authmode);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *e = data;
            xEventGroupClearBits(s_bits, NWIFI_ASSOCIATED | NWIFI_GOT_IP);
            xEventGroupSetBits(s_bits, NWIFI_DISCONNECTED);
            ESP_LOGI(TAG, "disconnected reason=%d", (int)e->reason);
            if (s_cb) s_cb(NET_EV_WIFI_DOWN);
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = data;
            xEventGroupSetBits(s_bits, NWIFI_GOT_IP);
            ESP_LOGI(TAG, "got ip " IPSTR " ms_to_ip=%d", IP2STR(&e->ip_info.ip), (int)((esp_timer_get_time() - s_connect_us) / 1000));
            if (s_cb) s_cb(NET_EV_GOT_IP);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(s_bits, NWIFI_GOT_IP);
            ESP_LOGI(TAG, "lost ip");
            if (s_cb) s_cb(NET_EV_LOST_IP);
        }
    }
}

static void zero(void *p, size_t n)
{
    volatile uint8_t *v = p;   // volatile so the compiler cannot drop the stores as dead
    while (n--) *v++ = 0;
}

// Undo what nwifi_start() did, as far as it got: stop and deinit the driver (when it was initialised), take the handlers off the event loop, free
// the netif. The default event loop and esp_netif_init() stay (neither has a matching undo that is safe to call), as does the event group.
static void teardown(bool driver_inited)
{
    if (driver_inited) {
        esp_wifi_stop();     // fine whether or not esp_wifi_start() was reached
        esp_wifi_deinit();
    }
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event);
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_cb = NULL;
    if (s_bits) xEventGroupClearBits(s_bits, NWIFI_ASSOCIATED | NWIFI_GOT_IP | NWIFI_DISCONNECTED);
}

void nwifi_stop(void) { teardown(true); }

static bool fail(const char *what, esp_err_t e, bool driver_inited)
{
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(e));
    teardown(driver_inited);
    return false;
}

// HOOK 1 (wifi_sta.h). The two profiles are separate names on purpose: the battery plan changes NET_PS_IDLE and nothing else. Default IDLE = ACTIVE.
// net_link.c power_profile_sync() calls the hook with IDLE while the chip may sleep (S2). IDLE stays MIN_MODEM on purpose: WIFI_PS_MAX_MODEM with
// the listen interval of 3 (set at association, below) is the spec's candidate, but no measurement of its downlink delay or of the link's survival
// over it exists, and a wrong guess shows up as a dial that drops its session while asleep. Flip this one line only after the bench A/B (gap report
// 2026-11-01-sleep-wifi-gaps.md, S2); test_wifi_power_source.py pins the current value so the flip is a deliberate edit of both.
#define NET_PS_ACTIVE WIFI_PS_MIN_MODEM
#define NET_PS_IDLE WIFI_PS_MIN_MODEM

bool net_power_profile(net_power_t profile)
{
    if (profile != NET_POWER_ACTIVE && profile != NET_POWER_IDLE) return false;
    const esp_err_t e = esp_wifi_set_ps(profile == NET_POWER_IDLE ? NET_PS_IDLE : NET_PS_ACTIVE);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_ps profile=%d: %s", (int)profile, esp_err_to_name(e));
    return e == ESP_OK;
}

bool nwifi_start(const char *ssid, const char *pass, nwifi_event_cb on_event_cb)
{
    if (!ssid || !*ssid || !pass) return false;
    if (!s_bits) s_bits = xEventGroupCreate();
    if (!s_bits) return false;
    s_cb = on_event_cb;

    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return fail("netif/event loop init", e, false);   // INVALID_STATE: the loop already exists
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return fail("netif create", ESP_ERR_NO_MEM, false);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[16];
    snprintf(hostname, sizeof hostname, "dial-%02x", mac[5]);
    esp_netif_set_hostname(s_netif, hostname);
    e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL);
    if (e != ESP_OK) return fail("event handlers", e, false);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;   // the driver neither reads nor writes NVS (FLASH SAFETY in wifi_sta.h); CONFIG_ESP_WIFI_NVS_ENABLED=n says the same at build time
    e = esp_wifi_init(&init);
    if (e != ESP_OK) return fail("esp_wifi_init", e, false);
    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials stay out of NVS (bench6 did the same)
    if (e != ESP_OK) return fail("esp_wifi_set_storage", e, true);
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return fail("esp_wifi_set_mode", e, true);

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    const size_t ssid_len = strnlen(ssid, sizeof wc.sta.ssid), pass_len = strnlen(pass, sizeof wc.sta.password - 1);
    memcpy(wc.sta.ssid, ssid, ssid_len);
    memcpy(wc.sta.password, pass, pass_len);
    wc.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;   // a minimum: WPA3 networks pass too
    wc.sta.pmf_cfg.capable = true;
    // The association request carries the listen interval (cable-wifi-link.md section 9, hook 1). It matters only to WIFI_PS_MAX_MODEM, which
    // nothing selects yet; 3 beacon intervals is the IDF default, written down so a later power hook finds it set once, here.
    wc.sta.listen_interval = 3;
    e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    zero(&wc, sizeof wc);
    if (e != ESP_OK) return fail("esp_wifi_set_config", e, true);
    ESP_LOGI(TAG, "config ssid_len=%u pass_len=%u authmode_min=%s storage=ram hostname=%s", (unsigned)ssid_len, (unsigned)pass_len,
             pass_len ? "wpa2_psk" : "open", hostname);

    // LOG LEVELS (wifi_sta.h): before the driver starts, so its first lines are already filtered.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);

    e = esp_wifi_start();   // first esp_phy_enable of the boot: may read and write the "phy" namespace, hence the internal-stack rule
    if (e != ESP_OK) return fail("esp_wifi_start", e, true);
    const bool ps_set = net_power_profile(NET_POWER_ACTIVE);   // min-modem: every number in the spec was measured with this
    ESP_LOGI(TAG, "started ps=min_modem listen_interval=3 (%s)", ps_set ? "ps set" : "ps not set");
    return true;
}

void nwifi_connect(void)
{
    xEventGroupClearBits(s_bits, NWIFI_DISCONNECTED);
    s_connect_us = esp_timer_get_time();
    const esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(e));   // CONN: a join is already under way
}

void nwifi_disconnect(void) { esp_wifi_disconnect(); }

uint32_t nwifi_bits(void) { return s_bits ? xEventGroupGetBits(s_bits) : 0; }

uint32_t nwifi_wait(uint32_t mask, uint32_t ms)
{
    if (!s_bits) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return 0;
    }
    return xEventGroupWaitBits(s_bits, mask, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
}

void nwifi_log_ap(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ESP_LOGI(TAG, "ap rssi=%d channel=%u", (int)ap.rssi, (unsigned)ap.primary);
}
