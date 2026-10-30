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

static const char *TAG = "wifi";

static EventGroupHandle_t s_bits;
static nwifi_event_cb s_cb;
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

bool nwifi_start(const char *ssid, const char *pass, nwifi_event_cb on_event_cb)
{
    if (!ssid || !*ssid || !pass) return false;
    s_cb = on_event_cb;
    s_bits = xEventGroupCreate();
    if (!s_bits) return false;

    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   // INVALID_STATE: the loop already exists
        ESP_LOGE(TAG, "netif/event loop init failed: %s", esp_err_to_name(e));
        return false;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
        ESP_LOGE(TAG, "no netif");
        return false;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[16];
    snprintf(hostname, sizeof hostname, "dial-%02x", mac[5]);
    esp_netif_set_hostname(netif, hostname);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;   // the driver neither reads nor writes NVS (FLASH SAFETY in wifi_sta.h)
    e = esp_wifi_init(&init);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(e));
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials stay out of NVS (bench6 did the same)
    esp_wifi_set_mode(WIFI_MODE_STA);

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
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "config ssid_len=%u pass_len=%u authmode_min=%s storage=ram hostname=%s", (unsigned)ssid_len, (unsigned)pass_len,
             pass_len ? "wpa2_psk" : "open", hostname);

    e = esp_wifi_start();   // first esp_phy_enable of the boot: may read and write the "phy" namespace, hence the internal-stack rule
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(e));
        return false;
    }
    e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // every number in the spec was measured with this
    ESP_LOGI(TAG, "started ps=min_modem listen_interval=3 (%s)", e == ESP_OK ? "ps set" : esp_err_to_name(e));
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
