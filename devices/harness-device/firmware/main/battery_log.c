#include "battery_log.h"

#include "battery.h"
#include "battery_ring.h"
#include "cable_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdio.h>

static const char *TAG = "batlog";
#define NS "batlog"
#define SAMPLE_MS 60000

// The ring's storage: one NVS blob per block, one for the meta record.
static bool blob_get(const char *key, void *buf, size_t cap, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) { *len = 0; return true; }   // the namespace does not exist yet
    if (e != ESP_OK) return false;
    size_t n = cap;
    e = nvs_get_blob(h, key, buf, &n);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) { *len = 0; return true; }
    if (e != ESP_OK) return false;
    *len = n;
    return true;
}
static bool blob_set(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    // Length 0 is "erase this block" (battery_ring.c drops a ring of another version); a key that is not there is erased already.
    esp_err_t e = len ? nvs_set_blob(h, key, buf, len) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND && !len) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static bool ring_load(int block, uint8_t *buf, size_t *len)
{
    char key[8];
    snprintf(key, sizeof key, "b%02d", block);
    return blob_get(key, buf, BATLOG_BLOCK * BATLOG_ENTRY_BYTES, len);
}
static bool ring_save(int block, const uint8_t *buf, size_t len)
{
    char key[8];
    snprintf(key, sizeof key, "b%02d", block);
    return blob_set(key, buf, len);
}
static bool meta_load(batlog_meta_t *m)
{
    size_t n = 0;
    return blob_get("meta", m, sizeof *m, &n) && n == sizeof *m;
}
static bool meta_save(const batlog_meta_t *m) { return blob_set("meta", m, sizeof *m); }
static const batlog_store_t s_store = {ring_load, ring_save, meta_load, meta_save};

static void emit(uint32_t seq, const batlog_entry_t *e, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "BATLOG seq=%u boot=%u up_s=%u pct=%u mv=%u shown=%d", (unsigned)seq, (unsigned)e->boot, (unsigned)e->up_s,
             (unsigned)e->pct, (unsigned)e->mv, battery_discharge_pct(e->pct));   // every sample was taken on battery (state 2)
}

// An internal stack on purpose: this task writes NVS, and the PSRAM-stack tasks (ptt, beep, proj_refresh) were moved on the
// grep result that nothing they reach does.
static void task(void *arg)
{
    (void)arg;
    const uint16_t boot = batlog_begin_boot(&s_store);
    ESP_LOGI(TAG, "battery log on: boot %u, one sample a minute while on battery", (unsigned)boot);
    bool dumped = false;
    int64_t next_us = esp_timer_get_time() + (int64_t)SAMPLE_MS * 1000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int chg = battery_chg();
        if (!dumped && chg >= 0 && chg != 2 && cable_client_is_connected()) {
            dumped = true;   // once a boot, when the dial is on external power and the daemon can file the lines
            ESP_LOGI(TAG, "BATLOG dump begin");
            int n = batlog_dump(&s_store, emit, NULL);
            ESP_LOGI(TAG, "BATLOG dump end n=%d", n);
        }
        const int64_t now = esp_timer_get_time();
        if (now < next_us) continue;
        next_us += (int64_t)SAMPLE_MS * 1000;
        if (next_us <= now) next_us = now + (int64_t)SAMPLE_MS * 1000;   // never a burst after a stall
        const int raw = battery_pct_raw();   // the gauge's own figure, not the remapped one shown on the glass
        if (chg != 2 || raw < 0) continue;
        const int mv = battery_mv();
        batlog_entry_t e = {boot, (uint32_t)(now / 1000000), (uint8_t)raw, (uint16_t)(mv > 0 ? mv : 0)};
        if (!batlog_append(&s_store, e)) ESP_LOGW(TAG, "NVS write failed; sample dropped");
    }
}

void battery_log_start(void)
{
    if (xTaskCreate(task, "batlog", 4096, NULL, 2, NULL) != pdPASS) ESP_LOGE(TAG, "task create failed");
}
