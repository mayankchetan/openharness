#include "runtime.h"
#include "touch.h"
#include "display.h"
#include "board.h"
#include "board_pins.h"
#include "board_i2c.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "power_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <assert.h>
#ifdef HARNESS_DEBUG_COMMANDS
#include "debug_cmd.h"
#endif
static esp_lcd_touch_handle_t controller;
static esp_lcd_panel_io_handle_t io;
static TaskHandle_t touch_task_handle;
static atomic_uint presses, failures, inferred, last_press;
static atomic_bool held, controller_ready;
// BEGIN battery sleep (touch)
// THE LEVEL WAKE (sleep plan section 3.3). While the chip may light-sleep the controller's INT pin (GPIO11, RTC-capable, active low)
// is the wake source: gpio_wakeup_enable(LOW_LEVEL) both wakes the chip and turns the pin's interrupt into a LEVEL interrupt. The
// touch driver owns that interrupt (esp_lcd_touch installs the only handler, touch_irq) and arms it for the FALLING EDGE. IDF does not
// put the type back (gpio_wakeup_disable leaves it alone), and a level interrupt stays asserted for as long as INT is low, so:
//   * touch_irq switches the interrupt off on its first hit while the wake is armed (s_level_wake) and notes it (s_irq_hit);
//   * the touch task reads the controller: a contact wakes the screen as ever (the sleep policy's screen hook then takes the awake
//     lock back, and the render task calls touch_sleep_disarm());
//   * no contact (the CST9217 also answers a stale ACK while idle): read again up to 3 times, 5 ms apart, then put the interrupt
//     back once INT is high; if INT stays low with nothing to read, park the wake until the next 1 s poll finds it high.
// No second GPIO handler is installed. The pad itself keeps its awake configuration in sleep (gpio_wakeup_enable does that).
#ifdef DEVICE_BATTERY_SLEEP
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "freertos/semphr.h"
#define TOUCH_INT_TYPE_AWAKE GPIO_INTR_NEGEDGE   // what the CST9217 driver arms (esp_lcd_touch_cst9217.c: levels.interrupt = 0)
static atomic_bool s_level_wake;     // armed as the wake source (set for S2)
static atomic_bool s_irq_hit;        // touch_irq switched the interrupt off since the last re-arm
static atomic_bool s_parked;         // armed, but the wake and the interrupt are off because INT would not go high
static atomic_uint s_unconfirmed;    // re-reads since the last hit
static atomic_llong s_isr_us;        // esp_timer time of the first hit since the last frame (0 = none): the start of the wake-to-glass time
static SemaphoreHandle_t s_pin_lock; // arm, disarm and re-arm never interleave
static inline void touch_sleep_irq(void)
{
    if (atomic_load_explicit(&s_level_wake, memory_order_relaxed)) {
        gpio_intr_disable(BSP_TOUCH_INT);
        atomic_store_explicit(&s_irq_hit, true, memory_order_relaxed);
        if (!atomic_load_explicit(&s_isr_us, memory_order_relaxed))
            atomic_store_explicit(&s_isr_us, (long long)esp_timer_get_time(), memory_order_relaxed);
    }
}
int64_t touch_sleep_take_isr_us(void)
{
    return (int64_t)atomic_exchange(&s_isr_us, 0);
}
static void touch_pin_arm(void)
{
    gpio_wakeup_enable(BSP_TOUCH_INT, GPIO_INTR_LOW_LEVEL);   // level type + wake source; the pad keeps its configuration in sleep
    esp_sleep_enable_gpio_wakeup();
    gpio_intr_enable(BSP_TOUCH_INT);
}
// True: the wake is armed (now, or already). False, and nothing armed: another chip, no pin lock, or the controller is not up (its
// driver was deleted after read errors and not reopened yet): the sleep policy then keeps the dial awake instead of letting the chip
// sleep with no way to wake it by touch.
bool touch_sleep_arm(void)
{
    if (board()->touch != TOUCH_CST9217 || !s_pin_lock || !atomic_load(&controller_ready))
        return false;
    xSemaphoreTake(s_pin_lock, portMAX_DELAY);
    if (!atomic_load(&s_level_wake)) {
        atomic_store(&s_irq_hit, false);
        atomic_store(&s_parked, false);
        atomic_store(&s_unconfirmed, 0);
        atomic_store(&s_isr_us, 0);
        atomic_store(&s_level_wake, true);   // 1: from here the ISR switches the interrupt off on its first hit
        touch_pin_arm();                     // 2: level wake
    }
    xSemaphoreGive(s_pin_lock);
    return true;
}
bool touch_sleep_armed(void)
{
    return atomic_load(&s_level_wake) && atomic_load(&controller_ready);
}
// The wake is parked: INT stayed low with nothing to read, so the wake and the interrupt are off and only the poll looks again.
static bool touch_sleep_parked(void)
{
    return atomic_load_explicit(&s_parked, memory_order_relaxed);
}
// The touch driver was deleted and opened again (open_touch): gpio_config and the new handler put the pin back to a falling-edge
// interrupt and the wake source may be gone. If the wake was armed, arm it again on the new driver.
static void touch_sleep_reopened(void)
{
    if (!s_pin_lock)
        return;
    xSemaphoreTake(s_pin_lock, portMAX_DELAY);
    if (atomic_load(&s_level_wake)) {
        atomic_store(&s_irq_hit, false);
        atomic_store(&s_parked, false);
        atomic_store(&s_unconfirmed, 0);
        touch_pin_arm();
    }
    xSemaphoreGive(s_pin_lock);
}
void touch_sleep_disarm(void)
{
    if (!s_pin_lock)
        return;
    xSemaphoreTake(s_pin_lock, portMAX_DELAY);
    if (atomic_load(&s_level_wake)) {
        gpio_intr_disable(BSP_TOUCH_INT);                          // 1: no interrupt of either kind while the pin changes over
        gpio_wakeup_disable(BSP_TOUCH_INT);                        // 2: not a wake source any more (IDF leaves the type alone)
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        gpio_set_intr_type(BSP_TOUCH_INT, TOUCH_INT_TYPE_AWAKE);   // 3: the driver's edge type back
        atomic_store(&s_level_wake, false);                        // 4: the ISR leaves the interrupt alone again
        atomic_store(&s_irq_hit, false);
        atomic_store(&s_parked, false);
        gpio_intr_enable(BSP_TOUCH_INT);                           // 5: and on
    }
    xSemaphoreGive(s_pin_lock);
}
// The touch task calls this after a sample that shows no contact. True: read again at once (the 5 ms pause is done here).
static bool touch_sleep_unconfirmed(void)
{
    if (!atomic_load(&s_level_wake))
        return false;
    if (atomic_load(&s_irq_hit) && atomic_load(&s_unconfirmed) < 3) {
        atomic_fetch_add(&s_unconfirmed, 1);
        vTaskDelay(pdMS_TO_TICKS(5));   // the controller may report the contact on the next read
        return true;
    }
    atomic_store(&s_unconfirmed, 0);
    xSemaphoreTake(s_pin_lock, portMAX_DELAY);
    if (atomic_load(&s_level_wake)) {   // (not disarmed meanwhile)
        if (gpio_get_level(BSP_TOUCH_INT)) {
            atomic_store(&s_irq_hit, false);
            if (atomic_exchange(&s_parked, false))
                touch_pin_arm();                       // the wake had been parked: arm it again
            else
                gpio_intr_enable(BSP_TOUCH_INT);       // the level interrupt, switched off by its first hit, back on
        } else if (!atomic_exchange(&s_parked, true)) {
            gpio_wakeup_disable(BSP_TOUCH_INT);        // INT is stuck low with nothing to read: let the chip sleep, look again on the next poll
            atomic_store(&s_irq_hit, false);           // (this hit is dealt with: the next poll goes straight to looking at INT)
        }
    }
    xSemaphoreGive(s_pin_lock);
    return false;
}
#else
static inline void touch_sleep_irq(void) {}
static inline bool touch_sleep_parked(void) { return false; }
static inline void touch_sleep_reopened(void) {}
#endif
// END battery sleep (touch)
static void touch_irq(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    touch_sleep_irq();
    BaseType_t wake = pdFALSE;
    if (touch_task_handle)
        vTaskNotifyGiveFromISR(touch_task_handle, &wake);
    if (wake)
        portYIELD_FROM_ISR();
}
static bool open_touch(void)
{
    const board_t *b = board();
    if (b->touch == TOUCH_NONE)
        return false;
    esp_lcd_panel_io_i2c_config_t c =
        b->touch == TOUCH_CST816S
            ? (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()
            : (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    c.scl_speed_hz = BSP_I2C_FREQ_HZ;
    if (esp_lcd_new_panel_io_i2c(board_i2c_get(), &c, &io) != ESP_OK)
        return false;
    esp_lcd_touch_config_t t = {.x_max = 466,
                                .y_max = 466,
                                .rst_gpio_num = b->touch_rst,
                                .int_gpio_num = BSP_TOUCH_INT,
                                .flags = {.mirror_x = b->touch_mirror, .mirror_y = b->touch_mirror},
                                .interrupt_callback = touch_irq};
    esp_err_t err = b->touch == TOUCH_CST816S ? esp_lcd_touch_new_i2c_cst816s(io, &t, &controller)
                                              : esp_lcd_touch_new_i2c_cst9217(io, &t, &controller);
    if (err != ESP_OK) {
        controller = NULL;
        esp_lcd_panel_io_del(io);
        io = NULL;
        return false;
    }
    atomic_store(&controller_ready, true);
    touch_sleep_reopened();   // (battery sleep only) a wake that was armed before the driver was deleted is armed again
    return true;
}
// How long the touch task waits for its next sample. A contact is read every 4 ms; an idle controller every 20 ms (the IRQ wakes the
// task at once, this only catches a missed one). While the chip may sleep the idle wait is the relaxed poll (power_policy.h).
static TickType_t touch_wait_ticks(bool down)
{
    if (down)
        return pdMS_TO_TICKS(4);
    uint32_t ms = power_sleep_wait_ms(PP_POLLER_TOUCH, 20);
    if (ms > 200 && touch_sleep_parked())
        ms = 200;   // the wake is parked (INT stuck low): no interrupt will come, so look five times a second, not once
    return pdMS_TO_TICKS(ms);
}
static void task(void *arg)
{
    (void)arg;
    bool prev = false, swallow = false;
    unsigned errors = 0;
    int64_t last_good = 0, retry_at = 0;
    uint16_t last_x = 0, last_y = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (!controller) {
#ifdef HARNESS_DEBUG_COMMANDS
            dbg_inject_abort();   // an injected contact needs this loop's controller path to exist
#endif
            if (now >= retry_at) {
                open_touch();
                retry_at = now + 5000000;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        uint16_t x = last_x, y = last_y, strength = 0;
        uint8_t count = 0;
        bool down = false, trusted = true, invalidate = true;
        esp_err_t rc = esp_lcd_touch_read_data(controller);
        if (rc == ESP_OK) {
            errors = 0;
            down = esp_lcd_touch_get_coordinates(controller, &x, &y, &strength, &count, 1) &&
                   count > 0;
            // A malformed controller coordinate must never become an action or a swipe.
            if (down && (x >= HT_WIDTH || y >= HT_HEIGHT)) {
                down = false;
                trusted = false;
            }
            if (down) {
                last_good = now;
                last_x = x;
                last_y = y;
            }
        } else if (rc == ESP_ERR_INVALID_RESPONSE && board()->touch == TOUCH_CST9217) {
            trusted = false;
            // This controller also returns stale ACKs while idle. With a previously verified
            // UP there is no contact to infer; ignore that idle sample, without swallowing the
            // next real DOWN. During a contact, however, any missing sample invalidates taps.
            invalidate = prev;
            // Preserve the proven stale-ACK recovery, expressed in time so a faster poll cannot
            // split a touch.
            errors = 0;
            down = prev && now - last_good < 100000;
            if (prev && !down)
                atomic_fetch_add(&inferred, 1);
        } else {
            trusted = false;
            atomic_fetch_add(&failures, 1);
            if (++errors >= 8) {
                atomic_store(&controller_ready, false);
                esp_lcd_touch_del(controller);
                controller = NULL;
                esp_lcd_panel_io_del(io);
                io = NULL;
                retry_at = now + 100000;
                errors = 0;
            }
        }
#ifdef HARNESS_DEBUG_COMMANDS
        {
            // A host-injected contact (debug.tap / press / swipe) stands in for the glass for this pass and
            // goes through everything below exactly as a finger would: the press count, the wake swallow,
            // the activity bump and habitat_touch(). Its coordinates were validated before it was armed.
            dbg_sample_t injected;
            if (dbg_inject_poll(now, &injected)) {
                down = injected.down;
                x = (uint16_t)injected.x;
                y = (uint16_t)injected.y;
                count = down ? 1 : 0;
                trusted = true;
                if (down) {
                    last_good = now;
                    last_x = x;
                    last_y = y;
                }
            }
        }
#endif
        if (!trusted && invalidate) {
            // Neither a stale ACK, a malformed coordinate nor a failed read is a tap release.
            // Do not re-arm on the next DOWN from the same still-held finger.
            display_lock();
            habitat_touch_cancel();
            display_unlock();
            swallow = swallow || prev || count > 0;
        }
        if (down && !prev) {
            atomic_fetch_add(&presses, 1);
#ifdef DEVICE_PERF_BENCH
            if (atomic_load(&presses) <= 16) ESP_LOGI("PERF", "touch raw x=%u y=%u", x, y);
#endif
            atomic_store(&last_press, (uint32_t)(now / 1000));
            if (!power_sleep_input_allowed()) {
                // D1, "PWR only": on the battery a dark screen is woken by the PWR key and by nothing else. The contact is dropped, whole:
                // no wake, no action, and (below) no activity stamp, so a pocket cannot keep the dial or the idle timer going.
                swallow = true;
            } else if (display_is_asleep()) {
                display_lock();
                habitat_touch_cancel();
                display_wake();
                display_unlock();
                swallow = true;
            } else
                habitat_input_stamp(now);
        }
        if (down && power_sleep_input_allowed())
            display_bump_activity();
        if (!swallow && (down || prev)) {
            display_lock();
            // PWR can put the screen to sleep halfway through a contact, even
            // while this task waits for the UI lock. Cancel under that same lock
            // and consume the rest of the contact, including after a PWR wake.
            if (display_is_asleep()) {
                habitat_touch_cancel();
                swallow = true;
            } else {
                habitat_touch(down, x, y, (uint32_t)(now / 1000));
            }
            display_unlock();
        }
        if (!down && trusted)
            swallow = false;
        prev = down;
        atomic_store(&held, down);
#ifdef DEVICE_BATTERY_SLEEP
        if (!down && touch_sleep_unconfirmed())   // woken by INT with no contact to show (yet)
            continue;
#endif
        // GPIO wakes immediately. Active samples have a 4ms fallback; idle fallback catches a
        // missed IRQ.
        ulTaskNotifyTake(pdTRUE, touch_wait_ticks(down));
    }
}
void touch_init(void)
{
#ifdef DEVICE_BATTERY_SLEEP
    s_pin_lock = xSemaphoreCreateMutex();   // without it the wake is never armed (touch_sleep_arm checks)
#endif
    assert(xTaskCreatePinnedToCore(task, "habitat_touch", 4096, NULL, 7, &touch_task_handle, 1) ==
           pdPASS);
}
uint32_t touch_activity_generation(void) { return atomic_load(&presses); }
void touch_stats(touch_stats_t *out)
{
    uint32_t pressed_at = atomic_load(&last_press);
    *out = (touch_stats_t){.presses = atomic_load(&presses),
                           .last_press_ms_ago =
                               (uint32_t)(esp_timer_get_time() / 1000) - pressed_at,
                           .read_failures = atomic_load(&failures),
                           .inferred_releases = atomic_load(&inferred),
                           .controller_ok = atomic_load(&controller_ready),
                           .held_now = atomic_load(&held)};
}
#ifdef DEVICE_BATTERY_SLEEP
void touch_wake_task(void)
{
    if (touch_task_handle)
        xTaskNotifyGive(touch_task_handle);
}
#endif
#ifdef HARNESS_DEBUG_COMMANDS
void touch_inject_wake(void)
{
    if (touch_task_handle)
        xTaskNotifyGive(touch_task_handle);
}
#endif
