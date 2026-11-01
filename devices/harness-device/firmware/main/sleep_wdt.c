#include "sleep_wdt.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "hal/rwdt_ll.h"
#include "hal/wdt_hal.h"
#include "hal/wdt_types.h"
#include "soc/rtc.h"

// The RTC watchdog (WDT_RWDT), configured and fed the way the IDF's own panic handler does it (esp_system/panic.c esp_panic_handler_enable_rtc_wdt and
// esp_panic_handler_feed_wdts). One stage, action RESET_SYSTEM (RTC memory survives). The write-protect register is shared by every write, so the three
// calls take a spinlock: the render task arms and disarms, the ptt task feeds. The IDF's light sleep entry leaves a watchdog that is already enabled
// alone (esp_hw_support/sleep_modes.c: "If WDT was enabled in the user code, then do not change it here"), and cpu_start disables it after an RWDT reset,
// so a reset caused by this does not repeat.
static wdt_hal_context_t s_ctx = RWDT_HAL_CONTEXT_DEFAULT();
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void sleep_wdt_arm(uint32_t ms)
{
    const uint32_t ticks = (uint32_t)((uint64_t)ms * rtc_clk_slow_freq_get_hz() / 1000ULL);
    portENTER_CRITICAL(&s_mux);
    wdt_hal_init(&s_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&s_ctx);
    // wdt_hal_init() turns "pause in sleep" ON (hal/wdt_hal_iram.c), and a watchdog that stops while the chip sleeps cannot catch a chip that never
    // wakes: the one thing this one is for (S8 review H1). Off again, here, with write protect off, before the stage is set.
    rwdt_ll_set_pause_in_sleep_en(s_ctx.rwdt_dev, false);
    wdt_hal_config_stage(&s_ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&s_ctx);
    wdt_hal_write_protect_enable(&s_ctx);
    portEXIT_CRITICAL(&s_mux);
}

void sleep_wdt_feed(void)
{
    portENTER_CRITICAL(&s_mux);
    wdt_hal_write_protect_disable(&s_ctx);
    wdt_hal_feed(&s_ctx);
    wdt_hal_write_protect_enable(&s_ctx);
    portEXIT_CRITICAL(&s_mux);
}

void sleep_wdt_disarm(void)
{
    portENTER_CRITICAL(&s_mux);
    wdt_hal_write_protect_disable(&s_ctx);
    wdt_hal_disable(&s_ctx);
    wdt_hal_write_protect_enable(&s_ctx);
    portEXIT_CRITICAL(&s_mux);
}
