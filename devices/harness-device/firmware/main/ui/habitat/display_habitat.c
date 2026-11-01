#include "illustrated.h"
// Direct text -> RGB565 -> QSPI DMA. No LVGL initialization or object pool.
#include "runtime.h"
#include "perf_bench.h"
#ifdef DEVICE_LAYOUT_BENCH
#include "layout_bench.h"
#endif
#ifdef DEVICE_TRANSPORT_BENCH
void cable_transport_benchmark(void);
#endif
#include "display.h"
#include "touch.h"
#include "ui_perf.h"
#include "audio_capture.h"
#include "cable_link.h"
#include "draw_buffer.h"
#include "ram_telemetry.h"
#include "battery.h"
#include "power_sleep.h"
#include "board_pins.h"
#include "board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifdef DEVICE_RENDER_FAULT
#include "esp_attr.h"
#include "cable_client.h"
#endif
#ifdef DEVICE_RENDER_STRESS
#include "audio_client.h"
#endif
#ifdef DEVICE_UI_METRICS
#include "uim.h"
#endif
#ifdef DEVICE_SLEEP_PROFILE
#include "sleep_profile.h"
static volatile display_slp_stamps_t s_slp;
void display_slp_stamps(display_slp_stamps_t *out) { *out = *(const display_slp_stamps_t *)&s_slp; }
#endif

#define STRIP_LINES 24

// ONE OF THE TWO DRAW BUFFERS IN PSRAM (plan C13 stage a). paint() alternates two 22,368 B strip buffers and starts every
// damage list on buffer 0, so a frame of one strip never touches PSRAM and a longer one sends every second strip from it:
// buffer 1 is the PSRAM one. 1 = that; 0 = both internal. On the dial (diet2-r1-results.md section 7): +22,532 B internal
// RAM, full-frame max +13 percent (25.1 to 28.4 ms), +27 percent against the same load without it under a PSRAM copy load.
// The panel IO is told to send a PSRAM colour buffer by DMA from PSRAM (psram_dma_direct). No padding is needed for the
// short last strip of a face (9,320 B): on the ESP32-S3 the transmit external-memory alignment of the SPI's AHB-GDMA channel
// is 1 byte (IDF 5.5.6 gdma.c gdma_config_transfer sets one only for the RX channel and for AHB-GDMA v2 / AXI), and
// spi_master.c setup_dma_priv_buffer bounces a transfer only when (address | length) & (alignment - 1) is not zero.
// The buffer itself is cache-line aligned and rounded up to a line (draw_buffer.c).
#ifndef HT_DRAWBUF_PSRAM
#define HT_DRAWBUF_PSRAM 1
#endif

// Stack of the render task. The ROM inflater keeps its Huffman tables on the calling task's stack, and an
// illustrated companion can become active at any time over the USB link, so a build that can draw one keeps
// 24,576 B. HABITAT_NO_COMPANION_ART (main/CMakeLists.txt, with HABITAT_FOCUS_ONLY) removes every path to the
// inflater: measured idle peak use there is 2,700 B (stack_free 21,876 of 24,576). 10,240 B is the interim
// value until the load measurement (reader paging, animation, recap, pet packs, OTA) names the real peak;
// the rule is peak + 4,096 B margin, rounded up to 1,024 B. Change it here only.
#ifdef HABITAT_NO_COMPANION_ART
#define RENDER_STACK_BYTES 10240
#else
#define RENDER_STACK_BYTES 24576
#endif
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t model_lock, dma_done;
static TaskHandle_t renderer;
static uint16_t *pixels[2];
static HT_RENDER_BSS ht_scene_t scenes[2];   // PSRAM by default (terminal.h HT_RENDER_DATA_PSRAM)
static bool painted;
static atomic_bool asleep, force_frame;
static atomic_uint wake_seq;   // display_wake_seq(): moves on a wake from off and on a dimmed screen coming back
// Panel IO belongs to the renderer, including brightness changes.
static atomic_uint requested_brightness = 40;
static atomic_uint last_activity;
static int64_t input_us;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static habitat_perf_t stats;
static void (*power_cb)(bool);
#ifdef DEVICE_BATTERY_SLEEP
static void (*sleep_cb)(bool);   // the sleep policy's hook (display_set_sleep_cb); called before power_cb on a wake
static atomic_bool screen_dimmed;   // the render task has dimmed the panel for the battery screen policy (power_policy.h)
#endif
// Watch actual renderer progress independently of the USB and touch tasks.
// A blocked panel call must not leave a responsive host controlling a frozen screen.
enum { RENDER_MODEL, RENDER_POWER, RENDER_DAMAGE, RENDER_RASTER, RENDER_DMA,
       RENDER_SUBMIT, RENDER_HEALTH, RENDER_WAIT };
static atomic_uint render_stage, render_progress_ms;
static esp_timer_handle_t render_guard;
// The CO5300's power-on register sequence. The Pro has no equivalent here: the ST7703's own
// init lives inside waveshare/esp_lcd_st7703 and is applied by esp_lcd_panel_init().
static const co5300_lcd_init_cmd_t init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t elapsed_since(const atomic_uint *stamp)
{
    // Read the shared timestamp FIRST. Sampling time before a concurrent store
    // can subtract a future millisecond and wrap into a false multi-day stall.
    uint32_t since = atomic_load(stamp);
    return now_ms() - since;
}
static void render_progress(unsigned stage)
{
    atomic_store(&render_progress_ms, now_ms());
    atomic_store(&render_stage, stage);
}
static void render_watch(void *arg)
{
    (void)arg;
    uint32_t elapsed = elapsed_since(&render_progress_ms);
    if (elapsed < 4000) return;
    static const char *stages[] = {"model", "panel power", "damage", "raster",
                                   "DMA wait", "panel submit", "health", "wake wait"};
    unsigned stage = atomic_load(&render_stage);
    ESP_LOGE("habitat", "renderer stalled %lu ms at %s; restarting", (unsigned long)elapsed,
             stage < sizeof stages / sizeof stages[0] ? stages[stage] : "unknown");
    // Panic reboot also preserves the stage in the existing last-words log.
    abort();
}
#ifdef DEVICE_BATTERY_SLEEP
// The stall guard is a 1 s periodic esp_timer, and an esp_timer alarm caps every light sleep at its period. While the chip may
// sleep (power_sleep.c, state S2) the render task stops it, and starts it again, on its own task, when the screen comes back.
// The task watchdog (10 s, feeding on every loop of the render task) still watches this task.
unsigned display_render_stage(void) { return atomic_load(&render_stage); }

void display_render_guard(bool run)
{
    static bool running = true;   // render_task starts it
    if (run == running)
        return;
    if (run) {
        render_progress(RENDER_MODEL);   // the stall clock starts now, not when the guard was stopped
        ESP_ERROR_CHECK(esp_timer_start_periodic(render_guard, 1000000));
    } else {
        ESP_ERROR_CHECK(esp_timer_stop(render_guard));
    }
    running = run;
}
#endif
#ifdef DEVICE_RENDER_FAULT
// Diagnostic image only. One deliberate blocked render task, then normal boot.
// The RTC marker survives the panic; no settings or NVS writes are involved.
static RTC_NOINIT_ATTR uint32_t injected_stall[2];
static void render_fault_once(void)
{
    const uint32_t cookie = 0x48524651u;
    if (injected_stall[0] == cookie && injected_stall[1] == ~cookie) return;
    if (now_ms() < 12000 || !cable_client_is_connected()) return;
    injected_stall[0] = cookie; injected_stall[1] = ~cookie;
    ESP_LOGW("render-test", "INJECT once: blocking renderer at panel submit; guard must recover");
    render_progress(RENDER_SUBMIT);
    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGE("render-test", "FAIL: renderer stall guard did not restart within 20 seconds");
    abort();
}
#endif
#ifdef DEVICE_RENDER_STRESS
// Local display-only soak. No fabricated contacts, app commands or microphone.
// A real touch or voice session cancels the test immediately.
static uint32_t render_stress_tick(void)
{
    static bool started, finished;
    static uint32_t due, cycles, rng = 0x41be95;
    if (finished) return 1000;
    uint32_t now = now_ms();
    if (now < 15000) return 20;
    touch_stats_t touch; touch_stats(&touch);
    if (touch.presses || audio_client_active()) {
        finished = true;
        ESP_LOGW("render-test", "STRESS cancelled by user input at cycle %lu", (unsigned long)cycles);
        return 1000;
    }
    if (!started) {
        started = true; due = now;
        ESP_LOGI("render-test", "STRESS begin: 1000 sleep/wake cycles; heap integrity + task stack checks");
    }
    if ((int32_t)(now - due) < 0) return 20;
    bool wake = display_is_asleep();
    display_lock();
    if (wake) display_wake(); else display_sleep();
    display_unlock();
    if (wake && ++cycles % 50 == 0) {
        assert(heap_caps_check_integrity_all(true));
        ESP_LOGI("render-test", "STRESS cycles=%lu int=%u min=%u psram=%u render_stack=%u touch_stack=%u action_stack=%u cable_stack=%u",
            (unsigned long)cycles, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)uxTaskGetStackHighWaterMark(NULL),
            (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("habitat_touch")),
            (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("habitat_actions")),
            (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("cable_link")));
    }
    if (cycles >= 1000) {
        finished = true;
        ESP_LOGI("render-test", "STRESS PASS: 1000 complete sleep/wake cycles, no heap corruption; normal control resumes");
        return 1000;
    }
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    due = now + 40 + rng % 181;
    return 20;
}
#endif
// True when the screen has been idle long enough to go off: 300 s on external power, 60 s on the battery (power_sleep.h).
static bool idle_expired(void) { return power_sleep_screen_level(elapsed_since(&last_activity)) == PP_SCREEN_OFF; }
void habitat_render_notify(void)
{
    if (renderer)
        xTaskNotifyGive(renderer);
}
void habitat_input_stamp(int64_t us)
{
    portENTER_CRITICAL(&stats_lock);
    input_us = us;
    portEXIT_CRITICAL(&stats_lock);
}
void habitat_perf_get(habitat_perf_t *out)
{
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
}
// THE DMA FENCE, in the two shapes the two panels report it.
//
// Both say the same thing — the pixels draw_bitmap was given have landed, so the buffer the CPU handed
// over is free again — and both give the same semaphore, which is the only thing the strip loop waits
// on. The dial reports it on the panel IO (a QSPI transfer finished); the Pro reports it on the DPI
// panel (a copy into the scanned framebuffer finished).
//
// On the Pro this MUST be on_color_trans_done and not on_refresh_done. The latter fires when the panel
// has finished SCANNING a frame, which is what a buffer-switch scheme waits for; ours copies, so
// waiting on the scan would fence against the wrong event and stall the first strip forever. The same
// distinction is written up at length in ui/panel_pro.c, which learned it the hard way.
static bool fence_release(void)
{
    ui_perf_flush_done();
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(dma_done, &wake);
    // Both drivers act on this return to decide whether to yield out of the ISR — the QSPI IO one and
    // the DPI one (esp_lcd_panel_dpi.c). Saying so is the contract; yielding here as well is not.
    return wake == pdTRUE;
}
static bool color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event,
                       void *ctx)
{
    (void)io;
    (void)event;
    (void)ctx;
    return fence_release();
}
void display_lock_at(const char *who)
{
    (void)who;
    if (model_lock)
        xSemaphoreTakeRecursive(model_lock, portMAX_DELAY);
}
void display_unlock(void)
{
    if (model_lock)
        xSemaphoreGiveRecursive(model_lock);
}
bool display_is_asleep(void) { return atomic_load(&asleep); }
uint32_t display_wake_seq(void) { return atomic_load_explicit(&wake_seq, memory_order_relaxed); }
void display_set_power_cb(void (*cb)(bool)) { power_cb = cb; }
#ifdef DEVICE_BATTERY_SLEEP
void display_set_sleep_cb(void (*cb)(bool)) { sleep_cb = cb; }
#endif
uint32_t display_idle_ms(void) { return elapsed_since(&last_activity); }

void display_bump_activity(void)
{
    atomic_store(&last_activity, now_ms());
#ifdef DEVICE_BATTERY_SLEEP
    if (atomic_load_explicit(&screen_dimmed, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&wake_seq, 1, memory_order_relaxed);   // a dimmed screen coming back is a wake too
        habitat_render_notify();   // a dimmed screen gets its brightness back at once, not at the end of the render wait
    }
#endif
}
void display_sleep(void)
{
    if (atomic_exchange(&asleep, true))
        return;
#ifdef DEVICE_SLEEP_PROFILE
    s_slp.sleep_us = esp_timer_get_time();
#endif
#ifdef DEVICE_BATTERY_SLEEP
    if (sleep_cb)
        sleep_cb(false);
#endif
    if (power_cb)
        power_cb(false);
    habitat_render_notify();
}
void display_wake(void)
{
    if (!atomic_exchange(&asleep, false))
        return;
    atomic_fetch_add_explicit(&wake_seq, 1, memory_order_relaxed);
#ifdef DEVICE_SLEEP_PROFILE
    s_slp.wake_us = esp_timer_get_time();
#endif
    display_bump_activity();
    atomic_store(&force_frame, true);
#ifdef DEVICE_BATTERY_SLEEP
    if (sleep_cb)
        sleep_cb(true);
#endif
    if (power_cb)
        power_cb(true);
    habitat_render_notify();
}
void display_set_brightness(uint8_t value)
{
    unsigned percent = ((unsigned)value * 100 + 127) / 255;
    atomic_store(&requested_brightness, percent < 8 ? 8 : percent);
    habitat_render_notify();
}
static void wait_dma(void)
{
    render_progress(RENDER_DMA);
    if (xSemaphoreTake(dma_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGE("habitat", "DMA completion timeout");
        abort();
    }
}
static uint32_t paint(const ht_scene_t *next, const ht_damage_t *damage)
{
    bool pending = false;
    int buffer = 0;
    int64_t started = esp_timer_get_time();
    uint32_t raster_us = 0;
    for (int n = 0; n < damage->count; n++) {
        ht_rect_t rect = damage->rect[n];
        // A small glyph edit fits in one transfer even when taller than a full-width strip.
        // Keep larger regions pipelined: a 40-row buffer saved a command but delayed their DMA.
        int step = rect.w * rect.h <= 2048 ? rect.h : STRIP_LINES;
        for (int y = rect.y; y < rect.y + rect.h; y += step) {
            int h = rect.y + rect.h - y;
            if (h > step)
                h = step;
            ht_rect_t strip = {rect.x, y, rect.w, h};
            int64_t t = esp_timer_get_time();
            render_progress(RENDER_RASTER);
#ifdef DEVICE_LAYOUT_BENCH
            ht_layout_raster(next, strip, pixels[buffer]);
#else
            ht_raster(next, strip, pixels[buffer]);
#endif
            raster_us += (uint32_t)(esp_timer_get_time() - t);
            // CPU prepares B while DMA owns A. Neither buffer is reused before the ISR fence.
            if (pending)
                wait_dma();
            ui_perf_flush((size_t)rect.w * h * 2);
            render_progress(RENDER_SUBMIT);
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, rect.x, y, rect.x + rect.w, y + h,
                                                      pixels[buffer]));
            pending = true;
            buffer ^= 1;
        }
    }
    if (pending)
        wait_dma();
    int64_t finished = esp_timer_get_time();
#ifdef DEVICE_OCTOPUS_BENCH
    octopus_perf_paint(raster_us, (uint32_t)(finished - started));
#endif
    portENTER_CRITICAL(&stats_lock);
    stats.frames++;
    stats.bytes += damage->pixels * 2;
    if (raster_us > stats.raster_max_us)
        stats.raster_max_us = raster_us;
    if (finished - started > stats.frame_max_us)
        stats.frame_max_us = (uint32_t)(finished - started);
    if (input_us && input_us <= started) {
        stats.input_last_us = (uint32_t)(finished - input_us);
        if (stats.input_last_us > stats.input_max_us)
            stats.input_max_us = stats.input_last_us;
        input_us = 0;
    }
    portEXIT_CRITICAL(&stats_lock);
#ifdef DEVICE_UI_METRICS
    uim_paint(raster_us, (uint32_t)(finished - started));
#endif
    return raster_us;
}
// The battery sleep fields of the alive line, only in a DEVICE_BATTERY_SLEEP image: the policy state the render task has applied
// (S0 external power, S1 battery with the screen on, S2 battery with the chip allowed to sleep), the light sleeps since boot and the
// milliseconds spent in them. A drain test reads sleep off the daemon's dial log from these: slp_ms rising by about 60,000 a minute
// means the chip sleeps almost all the time.
#ifdef DEVICE_BATTERY_SLEEP
#define PM_ALIVE_FMT " pm=%s slp=%lu slp_ms=%lu"
#define PM_ALIVE_ARGS , power_sleep_state_name(), (unsigned long)power_sleep_sleeps(), (unsigned long)power_sleep_slept_ms()
#else
#define PM_ALIVE_FMT ""
#define PM_ALIVE_ARGS
#endif
static void heartbeat(void)
{
    static uint32_t last;
    uint32_t now = now_ms();
    if (!last) {
        last = now;
        return;
    }
    if (now - last < 60000)
        return;
    last = now;
    touch_stats_t touch;
    habitat_perf_t perf;
    touch_stats(&touch);
    habitat_perf_get(&perf);
    // The host watches this exact prefix. Emit from the renderer itself, including while asleep,
    // so a live cable cannot mask a stalled UI. Nothing is drawn for this once-a-minute report.
    ESP_LOGI("habitat",
             "alive up=%lus heap=%u/%u psram=%u touches=%lu read_fails=%lu inferred=%lu "
             "frames=%lu bytes=%lu input_last_us=%lu muted=%d raster_max_us=%lu frame_max_us=%lu log_drops=%lu stack_free=%u "
             "mmap_remain=%d alloc_fail=%u bat=%d chg=%d mv=%d%s%s" PM_ALIVE_FMT,
             (unsigned long)(now / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)touch.presses, (unsigned long)touch.read_failures,
             (unsigned long)touch.inferred_releases, (unsigned long)perf.frames,
             (unsigned long)perf.bytes, (unsigned long)perf.input_last_us,
             (int)audio_notify_is_muted(), (unsigned long)perf.raster_max_us,
             (unsigned long)perf.frame_max_us, (unsigned long)cable_link_dropped_logs(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             ram_flash_mmap_remain_cached(), ram_telemetry_alloc_fail_count(),
             battery_pct(), battery_chg(), battery_mv(),   // -1 = unknown (no PMIC, or not polled yet); chg 0 standby, 1 charging, 2 on battery; mv the cell in mV
             touch.controller_ok ? "" : " TOUCH-DEAD",
             display_is_asleep() ? " asleep" : "" PM_ALIVE_ARGS);
    // The other nine tasks' stack high-water marks, on their own line (the alive line sits near the 512 B log limit).
    ram_telemetry_hwm();
    ram_telemetry_alloc_fail_flush();
}
#ifdef HARNESS_DEBUG_COMMANDS
// THE SCREENSHOT. The panel is write-only, so there is nothing to read back; the picture is the scene the
// panel is showing, rasterized a second time into the caller's buffer. That is exactly what paint() does
// for the panel, minus the QSPI transfer, and it is the cheapest honest answer: no full framebuffer is
// kept (the whole point of this renderer), and no strip is teed on the hot path, so a frame that nobody
// asked to see costs nothing.
//
// It runs on this task, between two frames, because ht_raster() owns two caches that only one renderer
// may touch, and because scenes[front] is only stable here. The cost is one extra full-face raster of
// this frame — about 20 strips of paint()'s own work — logged each time so it can be read off.
//
// A request is three states and one owner each: the caller moves IDLE -> REQUESTED, this task moves
// REQUESTED -> RUNNING -> IDLE. A caller that gives up may withdraw only a REQUESTED capture; once it is
// RUNNING the buffer is being written and stays the caller's problem until the semaphore comes back.
enum { CAPTURE_IDLE, CAPTURE_REQUESTED, CAPTURE_RUNNING };
static atomic_int capture_state;
static uint16_t *capture_out;
static dbg_err_t capture_result;
static SemaphoreHandle_t capture_lock, capture_done;
dbg_err_t display_capture(uint16_t *out, uint32_t timeout_ms)
{
    if (!out || !renderer || xSemaphoreTake(capture_lock, 0) != pdTRUE)
        return DBG_E_BUSY;
    xSemaphoreTake(capture_done, 0);   // a late give from an earlier capture that was withdrawn
    capture_out = out;
    atomic_store(&capture_state, CAPTURE_REQUESTED);
    habitat_render_notify();
    dbg_err_t result;
    if (xSemaphoreTake(capture_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        result = capture_result;
    } else {
        int requested = CAPTURE_REQUESTED;
        if (atomic_compare_exchange_strong(&capture_state, &requested, CAPTURE_IDLE)) {
            result = DBG_E_TIMEOUT;   // never started; nothing was written
        } else {
            xSemaphoreTake(capture_done, portMAX_DELAY);   // started: the render watchdog bounds this
            result = capture_result;
        }
    }
    xSemaphoreGive(capture_lock);
    return result;
}
static void capture_service(const ht_scene_t *scene, bool showing)
{
    int requested = CAPTURE_REQUESTED;
    if (!atomic_compare_exchange_strong(&capture_state, &requested, CAPTURE_RUNNING))
        return;
    // `showing`: the panel is on and has painted this scene. Asleep, or before the first frame, there is
    // no picture to take; the buffer is left as it was.
    capture_result = showing ? DBG_OK : DBG_E_ASLEEP;
    if (showing) {
        int64_t started = esp_timer_get_time();
        for (int y = 0; y < HT_HEIGHT; y += STRIP_LINES) {
            int h = HT_HEIGHT - y < STRIP_LINES ? HT_HEIGHT - y : STRIP_LINES;
            render_progress(RENDER_RASTER);
            ht_raster(scene, (ht_rect_t){0, y, HT_WIDTH, h}, capture_out + (size_t)y * HT_WIDTH);
        }
        ESP_LOGI("habitat", "screenshot raster %lu us", (unsigned long)(esp_timer_get_time() - started));
    }
    atomic_store(&capture_state, CAPTURE_IDLE);
    xSemaphoreGive(capture_done);
}
#endif
static void render_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
#ifdef DEVICE_TRANSPORT_BENCH
    cable_transport_benchmark();
#endif
#ifdef DEVICE_LAYOUT_BENCH
    ht_layout_benchmark(pixels[0], &scenes[0], &scenes[1], paint);
    portENTER_CRITICAL(&stats_lock);
    memset(&stats, 0, sizeof stats);
    portEXIT_CRITICAL(&stats_lock);
#endif
    render_progress(RENDER_MODEL);
    const esp_timer_create_args_t guard_args = {.callback = render_watch, .name = "render_guard"};
    ESP_ERROR_CHECK(esp_timer_create(&guard_args, &render_guard));
    ESP_ERROR_CHECK(esp_timer_start_periodic(render_guard, 1000000));
    int front = 0;
    bool panel_on = true;
    unsigned applied_brightness = 101;
    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
#ifdef DEVICE_BATTERY_SLEEP
        power_sleep_render_service();   // the sleep policy's actions that belong on this task (power_sleep.c)
#endif
#ifdef DEVICE_RENDER_FAULT
        render_fault_once();
#endif
#ifdef DEVICE_RENDER_STRESS
        uint32_t stress_wake_ms = render_stress_tick();
#endif
        render_progress(RENDER_MODEL);
        display_lock();
#ifdef DEVICE_OCTOPUS_BENCH
        int64_t model_started = esp_timer_get_time();
#endif
        habitat_tick();
        bool fresh = habitat_scene_take(&scenes[front ^ 1]);
#ifdef DEVICE_UI_METRICS
        if (fresh) uim_scene(habitat_view_name(), &scenes[front ^ 1]);   // counters only; the line is written elsewhere
#endif
        uint32_t receipt = habitat_scene_receipt();
        uint32_t wake_ms = habitat_next_wake_ms();
#ifdef DEVICE_RENDER_STRESS
        if (stress_wake_ms < wake_ms) wake_ms = stress_wake_ms;
#endif
#ifdef DEVICE_OCTOPUS_BENCH
        uint32_t model_us = (uint32_t)(esp_timer_get_time() - model_started);
        ht_perf_tag_t tag = octopus_perf_capture(&scenes[front ^ 1], fresh);
#endif
        display_unlock();
        unsigned brightness = atomic_load(&requested_brightness);
#ifdef DEVICE_BATTERY_SLEEP
        // The screen state is sampled ONCE for this pass (also used for `on` below): no panel writes while it is asleep, and on a wake
        // the user's brightness goes out in the pass that lights the panel, before the first frame.
        const bool awake = !display_is_asleep();
        {   // battery screen policy: dim after 30 s without a touch to a quarter of the user's setting (never below 3 percent), and
            // stay dimmed until the panel goes off at 60 s, so the glass does not flash bright just before it turns off. On external
            // power the level is only FULL or OFF: nothing was dimmed, so nothing stays dimmed.
            const pp_screen_t level = power_sleep_screen_level(elapsed_since(&last_activity));
            const bool dim = awake && (level == PP_SCREEN_DIM || (level == PP_SCREEN_OFF && atomic_load(&screen_dimmed)));
            atomic_store(&screen_dimmed, dim);
            if (dim)
                brightness = pp_dimmed_percent(brightness);
        }
#endif
        if (brightness != applied_brightness
#ifdef DEVICE_BATTERY_SLEEP
            && awake   // a sleeping panel is written again on the wake; a write now would only wake the bus
#endif
        ) {
            render_progress(RENDER_POWER);
            ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(panel, brightness));
            applied_brightness = brightness;
            ESP_LOGI("habitat", "OLED brightness %u%%", brightness);
        }
#ifdef DEVICE_BATTERY_SLEEP
        bool on = awake;   // the same sample as the brightness above; a wake that lands in between is picked up next pass (force_frame is kept)
#else
        bool on = !display_is_asleep();
#endif
        if (on != panel_on) {
            render_progress(RENDER_POWER);
            ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, on));
            panel_on = on;
#ifdef DEVICE_SLEEP_PROFILE
            if (!on) { s_slp.off_us = esp_timer_get_time(); s_slp.off_seq++; }
#endif
        }
        // A touch/PWR wake can arrive after we sampled `on`, including during
        // panel power-off. Keep its repaint request until an awake iteration.
        bool force = on && atomic_exchange(&force_frame, false);
        if (on && (fresh || force)) {
            if (!fresh)
                scenes[front ^ 1] = scenes[front];
            ht_damage_t damage;
            render_progress(RENDER_DAMAGE);
#ifdef DEVICE_OCTOPUS_BENCH
            int64_t damage_started = esp_timer_get_time();
#endif
            ht_illustrated_prepare(&scenes[front ^ 1]);
            ht_damage(painted && !force ? &scenes[front] : NULL, &scenes[front ^ 1], &damage);
#ifdef DEVICE_OCTOPUS_BENCH
            uint32_t damage_us = (uint32_t)(esp_timer_get_time() - damage_started);
#endif
            if (damage.count)
                paint(&scenes[front ^ 1], &damage);
#ifdef DEVICE_SLEEP_PROFILE
            if (force) { s_slp.frame_us = esp_timer_get_time(); s_slp.frame_seq++; }   // the full repaint after a wake is on the glass
#endif
#ifdef DEVICE_BATTERY_SLEEP
            if (force) {   // the full repaint after a wake is on the glass: how long since the touch interrupt woke the chip?
                const int64_t isr_us = touch_sleep_take_isr_us();
                if (isr_us)
                    ESP_LOGI("habitat", "wake isr to frame %lld us", (long long)(esp_timer_get_time() - isr_us));
            }
#endif
#ifdef DEVICE_OCTOPUS_BENCH
            octopus_perf_commit(tag, damage.pixels * 2, model_us, damage_us);
#endif
            front ^= 1;
            painted = true;
            if (receipt) habitat_scene_presented(receipt);
        }
        if (!on && fresh) {
            front ^= 1;
            painted = false;
        }
#ifdef HARNESS_DEBUG_COMMANDS
        capture_service(&scenes[front], on && painted);
#endif
        if (on && idle_expired()) {
            display_lock();
            // A touch may have refreshed activity while we waited for this
            // lock. The stale pre-lock idle decision must not swallow it.
            if (idle_expired())
                display_sleep();
            display_unlock();
        }
        render_progress(RENDER_HEALTH);
        heartbeat();
        // Notifications wake immediately; only finite reactions / voice require the short timeout.
        render_progress(RENDER_WAIT);
        // power_sleep_wait_ms: the same wait on external power and while the screen is on; the 5 s ceiling, with the animation
        // deadlines ignored, while the chip may sleep (the screen is off then, and a notification still wakes this task at once).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_RENDER, wake_ms > 1000 ? 1000 : wake_ms)));
    }
}
void display_init(void)
{
    ht_illustrated_init();
    model_lock = xSemaphoreCreateRecursiveMutex();
    dma_done = xSemaphoreCreateBinary();
    assert(model_lock && dma_done);
#ifdef HARNESS_DEBUG_COMMANDS
    capture_lock = xSemaphoreCreateMutex();
    capture_done = xSemaphoreCreateBinary();
    assert(capture_lock && capture_done);
#endif
    const spi_bus_config_t bus =
        CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_QSPI_SCLK, BSP_LCD_QSPI_D0, BSP_LCD_QSPI_D1,
                                     BSP_LCD_QSPI_D2, BSP_LCD_QSPI_D3, HT_WIDTH * STRIP_LINES * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg =
        CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_QSPI_CS, color_done, NULL);
    io_cfg.trans_queue_depth = 2;
#if HT_DRAWBUF_PSRAM
    io_cfg.flags.psram_dma_direct = 1;   // a colour buffer in PSRAM is sent from there; the internal one is not affected
#endif
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));
    co5300_vendor_config_t vendor = {.init_cmds = init_cmds,
                                     .init_cmds_size = sizeof(init_cmds) / sizeof(init_cmds[0]),
                                     .flags = {.use_qspi_interface = 1}};
    esp_lcd_panel_dev_config_t cfg = {.reset_gpio_num = board()->lcd_rst,
                                      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                      .bits_per_pixel = 16,
                                      .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io, &cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 6, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    bool buffer_in_psram[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        pixels[i] = draw_buffer_alloc(HT_WIDTH * STRIP_LINES * 2, HT_DRAWBUF_PSRAM && i == 1, &buffer_in_psram[i]);
        assert(pixels[i]);
    }
    display_bump_activity();
    assert(xTaskCreatePinnedToCore(render_task, "habitat_render", RENDER_STACK_BYTES, NULL, 5, &renderer, 1) ==
           pdPASS);
    touch_init();
    ESP_LOGI("habitat", "direct C renderer on a %dpx face: two %d-byte DMA buffers (%s, %s) over %s",
             HT_WIDTH, HT_WIDTH * STRIP_LINES * 2, buffer_in_psram[0] ? "PSRAM" : "internal",
             buffer_in_psram[1] ? "PSRAM" : "internal", "40MHz QSPI");
}
void display_init_ota(void) { display_init(); }
