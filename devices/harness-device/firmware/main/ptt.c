#include "ptt.h"
#include "ui/ui_screens.h"
#include "ui/display.h"
#include "board/power.h"
#include "battery.h"
#include "board/board_pins.h"
#include "board/board.h"
#include "audio_client.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "task_util.h"
#include "power_sleep.h"
#include "boot_key.h"

static const char *TAG = "ptt";
#define POLL_MS      100               // key poll cadence (latency only — the PMIC key event latches; BOOT is level-read)

// Two keys, two dials (board.h):
//   • BOOT (GPIO0, both dials), a state machine in boot_key.c:
//       - a TAP (released within BK_HOLD_MS) interrupts the running turn, on the release;
//       - a HOLD records: BOOT held for BK_HOLD_MS starts a voice capture through the same entry the touch gesture uses
//         (ui_voice_start), and the release sends it (ui_voice_stop). If no capture can start (a turn is running, no link, not a
//         talk screen) the hold is denied and its release is a tap;
//       - the screen toggle on a BOOT hold is gone where there is a PWR key, which does that; a board without the PMIC keeps it (a hold of
//         BK_TOGGLE_MS, and then no hold-to-talk);
//       - on the battery with the screen off BOOT does nothing at all (D1, "PWR only").
//     A key already down when this task starts (held through power-on) is ignored until it is released.
//   • Button A (the PWR key, wired to the AXP2101 PWRON pin — read over I2C, not a GPIO): a tap toggles
//     the screen. Only where there is an AXP2101; a long press restarts the device in PMIC hardware and
//     never reaches us. On the battery it is the only way to wake the dial (D1): the AXP2101 latches the press, so the
//     250 ms poll of the sleeping chip (PP_PWR_POLL_MS) cannot miss it.

static void screen_toggle(const char *why)
{
    // Hold the LVGL lock: display_sleep/wake touch LVGL timers + the panel, and normally run on the LVGL
    // task; the recursive mutex serialises this ptt-task call with it.
    display_lock();
    if (display_is_asleep()) { ESP_LOGI(TAG, "%s → screen ON",  why); display_wake(); }
    else                     { ESP_LOGI(TAG, "%s → screen OFF", why); display_sleep(); }
    display_unlock();
}

static void ptt_task(void *arg)
{
    gpio_config_t bcfg = {
        .pin_bit_mask = 1ULL << BSP_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&bcfg);
    const bool pwr_key = board()->has_pmic;
    bk_t boot_key;
    // Held through power-on: ignored until released. A board without the PWR key keeps BOOT's old second job, the screen toggle on a hold, and has no hold-to-talk.
    bk_init_ex(&boot_key, gpio_get_level(BSP_BOOT_BUTTON) == 0, !pwr_key);
    battery_poll_t battery_poll = {0};

    while (1) {
        // The one place the battery is read: percent, charge state and voltage, every 30 s, into battery.c's cache (the
        // alive line and the hello frame read that). Only a dial with the PMIC; the other boards keep -1 = unknown.
        if (pwr_key && battery_poll_due(&battery_poll, (uint32_t)(esp_timer_get_time() / 1000))) {
            battery_set(power_battery_pct(), power_charge_state());
            battery_set_mv(power_battery_mv());
        }
        power_sleep_poll();   // battery sleep policy (DEVICE_BATTERY_SLEEP; a no-op otherwise)
        if (pwr_key && power_take_pwrkey_tap()) screen_toggle("PWR tap");

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        switch (bk_step(&boot_key, now_ms, gpio_get_level(BSP_BOOT_BUTTON) == 0, power_sleep_input_allowed(), ui_voice_can_start)) {
        case BK_PRESS:
            if (display_is_asleep()) { display_lock(); display_wake(); display_unlock(); }   // external power only (D1 filtered the rest)
            break;
        case BK_TAP:
            ESP_LOGI(TAG, "BOOT tap → back / stop turn");
            ui_boot_pressed();
            break;
        case BK_HOLD_START:
            ESP_LOGI(TAG, "BOOT hold → record");
            ui_voice_start();
            break;
        case BK_HOLD_END:
            ESP_LOGI(TAG, "BOOT release → send");
            break;
        case BK_LONG:
            screen_toggle("BOOT hold");   // only a board without the PWR key gets here (toggle mode)
            break;
        case BK_NONE:
            break;
        }
        if (bk_stop_poll(&boot_key, now_ms, ui_voice_is_recording())) ui_voice_stop();   // right after the release, or when the capture begins

        vTaskDelay(pdMS_TO_TICKS(power_sleep_ptt_poll_ms(POLL_MS)));   // POLL_MS; 250 ms while the chip may sleep (power_sleep.h)
    }
}

void ptt_start(void)
{
    // Priority 3 — below the LVGL task (4) so button polling never preempts UI rendering.
#if TASK_STACK_PSRAM_PTT
    // Stack in PSRAM (task_util.h): the poll loop runs forever and calls no flash API (checked 2026-10-28).
    task_create_psram(ptt_task, "ptt", 4096, NULL, 3, NULL, tskNO_AFFINITY);
#else
    xTaskCreate(ptt_task, "ptt", 4096, NULL, 3, NULL);
#endif
}
