// BATTERY SLEEP, the ESP-IDF half. See power_sleep.h and briefs/research/2026-10-29-battery-sleep-plan.md.
#include "power_sleep.h"

#include "power_policy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#if !CONFIG_PM_ENABLE || !CONFIG_FREERTOS_USE_TICKLESS_IDLE
#error "DEVICE_BATTERY_SLEEP needs the options of sdkconfig.defaults.sleep (CONFIG_PM_ENABLE, CONFIG_FREERTOS_USE_TICKLESS_IDLE)"
#endif

static const char *TAG = "pm";

static esp_pm_lock_handle_t s_awake;   // ESP_PM_NO_LIGHT_SLEEP: held in S0 and S1, released in S2

void power_sleep_init(void)
{
    esp_err_t e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "dial_awake", &s_awake);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "dial_awake lock create failed (%s): the policy stays in S0", esp_err_to_name(e));
        s_awake = NULL;
        return;
    }
    esp_pm_lock_acquire(s_awake);   // nothing sleeps until the policy lets go of it
    ESP_LOGI(TAG, "battery sleep built in; policy S0 (awake lock held)");
}

bool power_sleep_pm_enter_battery(bool light)
{
    bool light_ok = light;
    if (light) {
        // esp_pm_configure ignores a failed retention allocation (pm_impl.c: esp_pm_sleep_configure's result is dropped) and
        // would sleep without CPU power-down, at a higher current and with one log line. Allocate first and look at the answer.
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
#if ESP_SLEEP_POWER_DOWN_CPU
        const esp_err_t r = esp_sleep_cpu_retention_init();
#else
        const esp_err_t r = ESP_OK;
#endif
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "sleep: no retention RAM (%s, largest block %u B): frequency scaling only", esp_err_to_name(r), (unsigned)largest);
            light_ok = false;
        }
    }
    const esp_pm_config_t cfg = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = light_ok};
    const esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure(240/80, light=%d) failed: %s", (int)light_ok, esp_err_to_name(e));
        return false;
    }
    return light_ok == light;
}

void power_sleep_pm_leave_battery(void)
{
    // Fixed 240 MHz, no light sleep: the same clocks as an image without power management. Also frees the retention RAM.
    const esp_pm_config_t cfg = {.max_freq_mhz = 240, .min_freq_mhz = 240, .light_sleep_enable = false};
    const esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) ESP_LOGE(TAG, "esp_pm_configure(240/240) failed: %s", esp_err_to_name(e));
}
