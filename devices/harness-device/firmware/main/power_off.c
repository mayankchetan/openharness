#include "power_off.h"

#ifdef DEVICE_BATTERY_POWEROFF

#include <stdatomic.h>
#include <string.h>

#include "battery.h"
#include "board/power.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "nvs.h"
#include "power_policy.h"
#include "store_gate.h"
#include "ui/display.h"
#include "ui/ui_screens.h"

static const char *TAG = "poff";

#define NS                  "poff"
#define KEY                 "rec"
#define REC_MAGIC           0x504F4646u   // "POFF"
#define FREEZE_TIMEOUT_MS   2000          // how long a writer in flight is waited for
#define SETTLE_MS           100           // after the last write has been committed, before the screen
#define SHOW_MS             1000          // "Off - hold PWR" is on the glass this long before the PMIC write
#define AFTER_MS            3000          // the PMIC takes the power within milliseconds; still running after this long means it did not
#define RETRY_US            (60LL * 1000000)   // an unforced request waits this long after a refusal

// What the next boot logs. 12 bytes, written whole.
typedef struct { uint32_t magic; uint8_t reason; int8_t pct; uint16_t mv; uint32_t up_s; } poff_rec_t;

static atomic_bool s_busy;
static int64_t s_next_try_us;
static int s_reason;
static bool s_force;

static const char *reason_name(int reason)
{
    return reason == POWER_OFF_REASON_DEBUG ? "debug" : pp_off_reason_name((pp_off_reason_t)reason);
}

static void write_record(int reason, int pct, int mv)
{
    const poff_rec_t rec = {.magic = REC_MAGIC, .reason = (uint8_t)reason, .pct = (int8_t)pct, .mv = (uint16_t)(mv > 0 ? mv : 0),
                            .up_s = (uint32_t)(esp_timer_get_time() / 1000000)};
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);   // not through the store gate: this is the one writer that runs while it is frozen
    if (e == ESP_OK) {
        e = nvs_set_blob(h, KEY, &rec, sizeof rec);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "POWEROFF: the record was not written (%d); powering off without it", (int)e);
}

// Remove the record (the writers are frozen or it is boot: this is the one writer allowed to run, so it does not go through the gate).
static void erase_record(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

// True when this is no time to power off: VBUS good (REG 0x00 bit 5), the register unreadable, or a USB host. Fresh, on this task: the cached state
// may be 30 s old. Logs the reason.
static bool external_power_now(const char *when)
{
    int s0 = -1, s1 = -1;
    power_pmu_read(&s0, &s1);
    if (s0 < 0 || (s0 & POWER_STATUS0_VBUS_GOOD)) {
        ESP_LOGW(TAG, "POWEROFF refused%s: VBUS good or REG 0x00 unreadable (reg00=%#x)", when, (unsigned)s0);
        return true;
    }
    if (usb_serial_jtag_is_connected()) {
        ESP_LOGW(TAG, "POWEROFF refused%s: a USB host is there", when);
        return true;
    }
    return false;
}

// Give the dial back: the record is erased (the dial is up, so the next boot must not report a power-off), then the writers run again and the
// home screen returns.
static void put_back(void)
{
    erase_record();
    store_gate_thaw();
    ui_leave_error_screen();
    s_next_try_us = esp_timer_get_time() + RETRY_US;
}

static void run(void)
{
    const int reason = s_reason, pct = battery_pct_raw(), mv = battery_mv();
#ifndef DEVICE_NO_OTA
    if (fw_update_active()) {
        ESP_LOGW(TAG, "POWEROFF refused: a firmware update is running");
        s_next_try_us = esp_timer_get_time() + RETRY_US;   // like every refusal: the policy asks again every poll, and a task a poll would be too many
        return;
    }
#endif
    if (!s_force && external_power_now("")) {
        s_next_try_us = esp_timer_get_time() + RETRY_US;
        return;
    }
    // The one line before the power goes (plan 16.1 item 2).
    ESP_LOGW(TAG, "POWEROFF reason=%s pct=%d mv=%d force=%d", reason_name(reason), pct, mv, (int)s_force);
    if (!store_gate_freeze(FREEZE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "POWEROFF not done: an NVS writer did not finish in %d ms", FREEZE_TIMEOUT_MS);
        s_next_try_us = esp_timer_get_time() + RETRY_US;
        return;
    }
    write_record(reason, pct, mv);   // committed before it returns; the writers are stopped, so nothing else is in the flash queue
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    display_lock();
    if (display_is_asleep()) display_wake();   // S2 has the screen off: the user is told why the dial went quiet
    display_unlock();
    ui_show_error("Power", "Off - hold PWR");
    vTaskDelay(pdMS_TO_TICKS(SHOW_MS));
    // The last look, right before the write: a cable can go in during the second the screen was up.
    if (!s_force && external_power_now(" after the screen")) {
        put_back();
        return;
    }
    const bool wrote = power_pmic_soft_off();   // REG 0x10 bit 0: on success the power goes within milliseconds and nothing below runs
    ESP_LOGW(TAG, "PMIC soft power-off written: %s", wrote ? "ok" : "FAILED");
    vTaskDelay(pdMS_TO_TICKS(AFTER_MS));
    // Still here: the PMIC did not take it (or VBUS keeps it up on a bench run). Put everything back.
    ESP_LOGE(TAG, "POWEROFF: still running %d ms after the PMIC write; the dial stays on", AFTER_MS);
    put_back();
}

static void task(void *arg)
{
    (void)arg;
    run();
    atomic_store(&s_busy, false);
    vTaskDelete(NULL);
}

bool power_off_request(int reason, bool force)
{
    if (!force && esp_timer_get_time() < s_next_try_us) return false;
    if (atomic_exchange(&s_busy, true)) return false;
    s_reason = reason;
    s_force = force;
    // An internal stack (xTaskCreate): the task writes NVS, and the ptt task that asks has a PSRAM stack that must not run a flash write.
    if (xTaskCreate(task, "poweroff", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "POWEROFF: task create failed");
        atomic_store(&s_busy, false);
        return false;
    }
    return true;
}

bool power_off_pending(void) { return atomic_load(&s_busy); }

void power_off_boot_report(void)
{
    // Raw bytes, for the bench gates P1-P3: REG 0x20 holds the power-on source bits and REG 0x21 the power-off source bits (datasheet V1.4 p.35-36).
    ESP_LOGI(TAG, "AXP2101 REG 0x20=%#x REG 0x21=%#x at boot (power-on and power-off source status)", (unsigned)power_read_reg(0x20),
             (unsigned)power_read_reg(0x21));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    poff_rec_t rec;
    size_t n = sizeof rec;
    const esp_err_t e = nvs_get_blob(h, KEY, &rec, &n);
    nvs_close(h);
    if (e != ESP_OK) return;   // nothing recorded: an ordinary boot
    if (n == sizeof rec && rec.magic == REC_MAGIC)
        ESP_LOGW(TAG, "previous boot ended in a PMIC power-off: reason=%s pct=%d mv=%u up_s=%u", reason_name(rec.reason), rec.pct, (unsigned)rec.mv,
                 (unsigned)rec.up_s);
    erase_record();
}

#endif
