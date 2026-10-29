#include "sleep_profile.h"

#include "cable_client.h"
#include "cable_debug.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ram_telemetry.h"
#include "slp_stats.h"
#include "ui/display.h"
#include "ui/touch.h"

static const char *TAG = "slp";

enum { CYCLES = 500, WINDOW = 50, START_S = 20, HOLD_ASLEEP_MS = 1000, SETTLE_MS = 500, STEP_TIMEOUT_MS = 5000 };

// ── boot profile ─────────────────────────────────────────────────────────────────────────────────────────────────────
// Recorded on the main task at app_ready and just before app_main returns; logged again when the run starts (the first
// lines go out before the daemon session exists and reach only the raw console).
static struct { uint32_t ready_ms, ready_stack, ready_int_min, ready_int_free, end_stack; bool have_ready, have_end; } s_boot;

void sleep_profile_app_ready(void)
{
    s_boot.ready_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_boot.ready_stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_boot.ready_int_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_boot.ready_int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_boot.have_ready = true;
    ESP_LOGI(TAG, "BOOT app_ready t_ms=%u main_stack_free_min=%u int_min=%u int_free=%u", (unsigned)s_boot.ready_ms,
             (unsigned)s_boot.ready_stack, (unsigned)s_boot.ready_int_min, (unsigned)s_boot.ready_int_free);
}

void sleep_profile_main_end(void)
{
    s_boot.end_stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_boot.have_end = true;
    ESP_LOGI(TAG, "BOOT main_end main_stack_free_min=%u", (unsigned)s_boot.end_stack);
}

static void log_boot(void)
{
    ESP_LOGI(TAG, "BOOT app_ready t_ms=%u (esp_timer, from the start of the app, not the ROM and bootloader) "
             "main_stack_free_min=%u (stack 10240) int_min=%u int_free=%u; at main end main_stack_free_min=%u",
             (unsigned)s_boot.ready_ms, (unsigned)s_boot.ready_stack, (unsigned)s_boot.ready_int_min,
             (unsigned)s_boot.ready_int_free, (unsigned)s_boot.end_stack);
}

// ── the cycles ───────────────────────────────────────────────────────────────────────────────────────────────────────
static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static int mem_free(void) { return (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static int mem_largest(void) { return (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void run(void)
{
    touch_stats_t touch;
    touch_stats(&touch);
    if (!touch.controller_ok) {
        ESP_LOGE(TAG, "SLP ABORT touch controller not ready: the wake must come through the touch path");
        return;
    }
    log_boot();
    ESP_LOGI(TAG, "SLP START cycles=%d: sleep (display_sleep under the display lock, as the idle timer and the PWR key), "
             "hold %d ms, wake by an injected tap through the touch task, wait for the first full frame", CYCLES, HOLD_ASLEEP_MS);
    static int32_t wake[WINDOW], sleep_[WINDOW];   // signed microseconds
    int in_window = 0, first_free = 0, first_largest = 0, last_free = 0, last_largest = 0, window_first = 1, failures = 0, consecutive = 0, excluded = 0, ext_total = 0;
    for (int n = 1; n <= CYCLES; n++) {
        display_slp_stamps_t before;
        display_slp_stamps(&before);
        // The path of the 300 s idle timer and of the PWR key: the display lock, then display_sleep().
        const int64_t t_sleep = esp_timer_get_time();
        display_lock();
        display_sleep();
        display_unlock();
        int64_t waited = 0;
        display_slp_stamps_t st;
        do { wait_ms(5); display_slp_stamps(&st); waited += 5000; } while (st.off_seq == before.off_seq && waited < STEP_TIMEOUT_MS * 1000LL);
        const bool off = st.off_seq != before.off_seq;
        wait_ms(HOLD_ASLEEP_MS);
        // The wake: a tap played by the touch task as a finger would be (the press while asleep wakes the panel and is
        // swallowed). Retry while a host debug gesture is in flight.
        const int64_t t_wake = esp_timer_get_time();
        bool armed = false;
        for (int tries = 0; tries < 200 && !armed; tries++) {
            armed = cable_debug_inject_tap(233, 233);
            if (!armed) wait_ms(10);
        }
        waited = 0;
        do { wait_ms(2); display_slp_stamps(&st); waited += 2000; } while (st.frame_seq == before.frame_seq && waited < STEP_TIMEOUT_MS * 1000LL);
        const bool framed = armed && st.frame_seq != before.frame_seq;
        // Signed microseconds. sleep: from just before the display lock to the moment the renderer's panel-off command
        // returned (sleep_core: from display_sleep() taking effect). wake: from arming the tap (core: from display_wake()
        // taking effect) to the first full frame painted and DMA-complete.
        const int32_t sleep_us = off ? (int32_t)(st.off_us - t_sleep) : 0;
        const int32_t sleep_core_us = off ? (int32_t)(st.off_us - st.sleep_us) : 0;
        const int32_t wake_us = framed ? (int32_t)(st.frame_us - t_wake) : 0;
        const int32_t core_us = framed ? (int32_t)(st.frame_us - st.wake_us) : 0;
        // Something else woke the panel before our tap (a notification, the daemon, a touch): the first frame precedes the
        // arm point, the tap landed on an awake face, and this cycle says nothing about the wake path. Flag it, leave it
        // out of the statistics (the first run showed one as an unsigned wrap).
        const bool external = framed && (st.wake_us < t_wake || st.frame_us < t_wake);
        if (!off || !framed) {
            failures++;
            consecutive++;
            ESP_LOGW(TAG, "SLP cycle=%d FAILED panel_off=%d tap_armed=%d first_frame=%d", n, off, armed, framed);
            if (consecutive >= 3) {
                ESP_LOGE(TAG, "SLP ABORT three cycles in a row failed; waking the panel and stopping");
                display_lock(); display_wake(); display_unlock();
                return;
            }
        } else consecutive = 0;
        wait_ms(SETTLE_MS);   // the injected tap lifts; the face settles
        const int free_now = mem_free(), largest_now = mem_largest();
        if (n == 1) { first_free = free_now; first_largest = largest_now; }
        last_free = free_now; last_largest = largest_now;
        ESP_LOGI(TAG, "SLP cycle=%d wake_ms=%d core_ms=%d sleep_ms=%d wake_us=%d core_us=%d sleep_us=%d sleep_core_us=%d "
                 "ext_wake=%d int_free=%d int_largest=%d psram_free=%u alloc_fail=%u", n,
                 (int)(wake_us / 1000), (int)(core_us / 1000), (int)(sleep_us / 1000), (int)wake_us, (int)core_us, (int)sleep_us,
                 (int)sleep_core_us, (int)external, free_now, largest_now,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)ram_telemetry_alloc_fail_count());
        if (external) { excluded++; ext_total++; }
        else if (off && framed) { wake[in_window] = wake_us; sleep_[in_window] = sleep_us; in_window++; }
        if (n % WINDOW == 0) {
            char line[300];
            slp_format_summary(line, sizeof line, window_first, n, wake, sleep_, in_window, excluded, free_now, first_free, largest_now,
                               (int)ram_telemetry_alloc_fail_count());
            ESP_LOGI(TAG, "%s", line);
            in_window = 0;
            excluded = 0;
            window_first = n + 1;
            ram_telemetry_alloc_fail_flush();
        }
    }
    ESP_LOGI(TAG, "SLP DONE cycles=%d failures=%d ext_wakes=%d int_free_first=%d int_free_last=%d largest_first=%d largest_last=%d", CYCLES,
             failures, ext_total, first_free, last_free, first_largest, last_largest);
}

static void task(void *arg)
{
    (void)arg;
    while (esp_timer_get_time() < (int64_t)START_S * 1000000) wait_ms(200);
    // Wait (up to a minute) for the daemon session: the lines go to the dial log only through it.
    for (int i = 0; i < 60 && !cable_client_is_connected(); i++) wait_ms(1000);
    run();
    vTaskDelete(NULL);
}

void sleep_profile_start(void)
{
    ESP_LOGW(TAG, "SLEEP PROFILE image: %d sleep/wake cycles start %d s after boot; not a product build", CYCLES, START_S);
    if (xTaskCreate(task, "slp", 4096, NULL, 3, NULL) != pdPASS) ESP_LOGE(TAG, "task create failed");
}
