// BATTERY SLEEP, the ESP-IDF half. See power_sleep.h and briefs/research/2026-10-29-battery-sleep-plan.md.
//
// Who does what (the plan's section 3.1):
//   ptt task      power_sleep_poll(): reads the inputs (a fresh PMIC read every 2 s when it matters), steps the policy
//                 (power_policy.c) and writes the state it wants. Takes the awake lock back at once when the dial must stay up.
//   render task   power_sleep_render_service(): applies that state. It has an internal stack and the cache-safe context the
//                 power-management calls want (ptt's stack is in PSRAM), and it is the task that owns the panel.
//   any task      on_screen(): the screen woke. Takes the awake lock back before anything else.
//
// The awake lock "dial_awake" (ESP_PM_NO_LIGHT_SLEEP) is held in S0 and S1 and released only in S2. Its hold is a binary
// ownership (s_lock_held), so the three places that may take it back (ptt, the screen hook, the render service) cannot count it twice.
#include "power_sleep.h"

#include "audio_capture.h"
#include "audio_client.h"
#include "battery.h"
#include "board/board.h"
#include "board/power.h"
#include "board/sleep_pins.h"
#include "cable_client.h"
#include "cable_link.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "fw_update.h"
#include "power_policy.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "ui/display.h"
#include "ui/habitat/runtime.h"
#include "ui/touch.h"
#include <stdatomic.h>
#ifdef DEVICE_BATTERY_POWEROFF
#include "power_off.h"
#endif

#if !CONFIG_PM_ENABLE || !CONFIG_FREERTOS_USE_TICKLESS_IDLE
#error "DEVICE_BATTERY_SLEEP needs the options of sdkconfig.defaults.sleep (CONFIG_PM_ENABLE, CONFIG_FREERTOS_USE_TICKLESS_IDLE)"
#endif

static const char *TAG = "pm";

// What the chip really slept, from the exit callback of automatic light sleep (CONFIG_PM_LIGHT_SLEEP_CALLBACKS, in the overlay).
// The callback runs from the idle task with interrupts off, under the power-management switch lock, so one writer at a time.
// A sleep that was rejected returns at once and is not counted. The alive line prints the two numbers.
static volatile uint32_t s_slept_count;
static volatile uint32_t s_slept_ms;
static volatile uint32_t s_slept_rem_us;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static esp_err_t IRAM_ATTR on_light_sleep_exit(int64_t slept_us, void *arg)
{
    (void)arg;
    if (slept_us >= 1000) {
        s_slept_count++;
        s_slept_rem_us += (uint32_t)slept_us;
        s_slept_ms += s_slept_rem_us / 1000;
        s_slept_rem_us %= 1000;
    }
    return ESP_OK;
}
#endif

// 1: the S0 to S1 step also turns automatic light sleep on (CPU and cache retention RAM, about 13.4 KB, allocated once for the whole
// battery session and freed when external power returns); 0: frequency scaling only. If the RAM is not there the dial stays on 1.
#define POWER_SLEEP_LIGHT 1

static esp_pm_lock_handle_t s_awake;   // ESP_PM_NO_LIGHT_SLEEP: held in S0 and S1, released in S2
static atomic_bool s_lock_held;        // who owns the lock right now: flipped by exchange, so one acquire pairs with one release
static atomic_bool s_relaxed;          // the pollers run at their S2 rates (set when S2 is entered, cleared the moment it is left)
static atomic_bool s_in_s2;            // S2 is applied (render task) or being applied: the screen hook takes the lock back while this is set
static atomic_int  s_want;             // the policy's state (pp_state_t), written by the ptt task
static atomic_uint s_link_holds;       // PP_HOLD_LINK and friends, set by transports through power_sleep_hold()
static pp_t        s_pp;               // the policy; the ptt task only
#ifdef DEVICE_BATTERY_POWEROFF
static pp_off_t    s_off;              // the PMIC power-off triggers (power_policy.h pp_off_step); the ptt task only
static bool        s_last_on_battery;  // the last fresh PMIC read said "on the battery" (pp_on_battery)
#endif
static pp_state_t  s_applied;          // what the render task has applied
static bool        s_pm_battery;       // power management is configured for battery (frequency scaling, maybe light sleep)
static bool        s_audio_idle;       // the I2S channels are stopped (audio_capture_idle)
static bool        s_s2_live;          // the S2 steps that let the chip sleep are done: touch wake armed, pollers relaxed, lock released
static int         s_last_status0 = -1, s_last_status1 = -1;   // the raw AXP2101 bytes of the last fresh read (-1: it failed), for the log
#if PP_TOUCH_WAKE_ON_BATTERY
static bool        s_touch_warned;     // "touch wake not armed" is logged once per boot, not once per retry
#endif

static int resolve_gpio(int gpio)
{
    if (gpio == SLEEP_PIN_LCD_RST) return board()->lcd_rst;
    if (gpio == SLEEP_PIN_TOUCH_RST) return board()->touch_rst;
    return gpio;
}

// Every output the panel and the amplifier depend on keeps its awake level through light sleep (sleep_pins.h says which).
// Done once; gpio_sleep_sel_dis() only matters while the chip sleeps, so it changes nothing while awake.
static void keep_output_pads(void)
{
    int kept = 0;
    for (int i = 0; i < SLEEP_PIN_COUNT; i++) {
        if (SLEEP_PINS[i].light != SLEEP_LIGHT_KEEP) continue;
        const int gpio = resolve_gpio(SLEEP_PINS[i].gpio);
        if (gpio < 0) continue;
        if (gpio_sleep_sel_dis((gpio_num_t)gpio) == ESP_OK) kept++;
    }
    ESP_LOGI(TAG, "%d output pads keep their level in sleep", kept);
}

// ── the awake lock ──────────────────────────────────────────────────────────────────────────────────

static void lock_acquire_once(void)
{
    if (s_awake && !atomic_exchange(&s_lock_held, true)) esp_pm_lock_acquire(s_awake);
}
static void lock_release_once(void)
{
    if (s_awake && atomic_exchange(&s_lock_held, false)) esp_pm_lock_release(s_awake);
}

// ── power management for battery (frequency scaling, and light sleep once the retention RAM is there) ──

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

// ── init ────────────────────────────────────────────────────────────────────────────────────────────

static void on_screen(bool on);

void power_sleep_init(void)
{
    keep_output_pads();
    pp_init(&s_pp);
#ifdef DEVICE_BATTERY_POWEROFF
    pp_off_init(&s_off);
#endif
    atomic_store(&s_want, PP_S0);
    s_applied = PP_S0;
    esp_err_t e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "dial_awake", &s_awake);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "dial_awake lock create failed (%s): the policy stays in S0", esp_err_to_name(e));
        s_awake = NULL;
        return;
    }
    lock_acquire_once();   // nothing sleeps until the policy lets go of it
    display_set_sleep_cb(on_screen);
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {.exit_cb = on_light_sleep_exit};
    if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) ESP_LOGW(TAG, "light sleep callback not registered: slp= stays 0");
#endif
    ESP_LOGI(TAG, "battery sleep built in; policy S0 (awake lock held)");
}

// ── the ptt task ────────────────────────────────────────────────────────────────────────────────────

static bool board_capable(void) { return board()->has_pmic && board()->touch == TOUCH_CST9217; }

static uint32_t holds_now(void)
{
    uint32_t holds = atomic_load_explicit(&s_link_holds, memory_order_relaxed);
    if (audio_client_active()) holds |= PP_HOLD_VOICE;
#ifndef DEVICE_NO_OTA
    if (fw_update_active()) holds |= PP_HOLD_FWUPDATE;
#endif
    // A USB cable session holds the chip awake (a host whose SOF flickers). A Wi-Fi session does not: modem sleep keeps the association and the
    // TLS socket lives in RAM, which light sleep keeps. The window from "link up" to "session up" is the link hold's (net_link.c serve()).
    if (cable_client_is_connected() && cable_link_transport() == CABLE_XPORT_USB) holds |= PP_HOLD_SESSION;
    return holds;
}

static void on_hold(void);

void power_sleep_hold(uint32_t bits)
{
    const uint32_t before = atomic_fetch_or(&s_link_holds, bits);
    if ((before & bits) != bits) on_hold();
}
void power_sleep_release(uint32_t bits) { atomic_fetch_and(&s_link_holds, ~bits); }

void power_sleep_poll(void)
{
    if (!s_awake) return;   // the lock could not be made: stay in S0
    pp_in_t in = {
        .now_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .chg = battery_chg(),
        .chg_fresh = false,
        .capable = board_capable(),
        .sof = usb_serial_jtag_is_connected(),
        .screen_asleep = display_is_asleep(),
        .holds = holds_now(),
    };
    int status1 = -1;
    if (pp_wants_read(&s_pp, &in)) {
        // One fresh read of REG 0x00 (VBUS good, battery present) and REG 0x01 (battery current direction), on this task's own I2C
        // path. "On the battery" needs all three (power_policy.h); a failed read of either is not.
        in.chg = power_pmu_read(&in.status0, &status1);
        in.chg_fresh = true;
        s_last_status0 = in.status0;
        s_last_status1 = status1;
        battery_set(battery_pct_raw(), in.chg);   // the 30 s cache follows what was just read: alive line, hello frame, battery log
#ifdef DEVICE_BATTERY_POWEROFF
        s_last_on_battery = pp_on_battery(&in);
#endif
    }
    pp_out_t out;
    pp_step(&s_pp, &in, &out);
#ifdef DEVICE_BATTERY_POWEROFF
    {
        // S3: from S2 only, never on external power. The sequence runs on its own task (power_off.c) and re-reads VBUS before it acts.
        const pp_off_in_t oin = {.now_ms = in.now_ms, .state = out.state, .on_battery = s_last_on_battery, .sof = in.sof, .pct = battery_pct_raw()};
        const pp_off_reason_t why = pp_off_step(&s_off, &oin);
        if (why != PP_OFF_NONE && power_off_request((int)why, false))
            ESP_LOGW(TAG, "power-off asked for: %s (state %s, pct %d)", pp_off_reason_name(why), pp_state_name(out.state), oin.pct);
    }
#endif
    if (out.action == PP_ACT_NONE) return;

    ESP_LOGI(TAG, "%s -> %s (chg=%d reg00=%#x reg01=%#x sof=%d asleep=%d holds=0x%x) int_free=%u largest=%u", pp_state_name(out.from),
             pp_state_name(out.state), in.chg, (unsigned)s_last_status0, (unsigned)s_last_status1, (int)in.sof, (int)in.screen_asleep, (unsigned)in.holds,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    atomic_store(&s_want, out.state);
    if (out.action == PP_ACT_LEAVE_S2 || out.action == PP_ACT_ENTER_S0) {
        // The dial must stay up (the screen woke, work started, or external power came back): take the lock back and stop
        // relaxing the pollers now, on this task. The render task undoes the rest on its next loop.
        lock_acquire_once();
        atomic_store(&s_relaxed, false);
    }
    habitat_render_notify();
}

// ── the screen hook (any task) ──────────────────────────────────────────────────────────────────────

static void on_screen(bool on)
{
    // The screen woke while the chip was allowed to sleep: take the awake lock back before anything else, so the chip does not go
    // back to sleep under a finger. The render task restores the rest when it sees the screen on.
    if (on && atomic_load(&s_in_s2)) {
        lock_acquire_once();
        atomic_store(&s_relaxed, false);
        touch_wake_task();   // the touch task sits in its 5 s S2 wait: it reads the glass now, not at the end of the wait
    }
}

// A new hold while the chip may sleep (S2 applied, the lock let go): take the lock back NOW, from the task that asked, and stop relaxing the
// pollers. Without this the lock would come back at the next ptt poll, up to 250 ms later, and the first part of a join or a handshake could run
// with light sleep allowed. The policy sees the hold at its next poll and moves to S1; if the hold has gone by then, the render task lets the
// lock go again (power_sleep_render_service). Idempotent, like on_screen().
static void on_hold(void)
{
    if (!atomic_load(&s_in_s2)) return;
    lock_acquire_once();
    atomic_store(&s_relaxed, false);
}

// ── the render task ─────────────────────────────────────────────────────────────────────────────────

static void enter_battery(void)
{
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool light = power_sleep_pm_enter_battery(POWER_SLEEP_LIGHT);
    s_pm_battery = true;
    // G2b of the plan: the retention RAM should show here as about 13.4 KB less free (and the largest block before it at least 64 KB)
    ESP_LOGI(TAG, "battery: %s; int_free %u -> %u, largest %u -> %u", light ? "light sleep on" : "frequency scaling only", (unsigned)free_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)largest_before, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void leave_battery(void)
{
    if (s_pm_battery) power_sleep_pm_leave_battery();
    s_pm_battery = false;
}

// Let the chip sleep: relax the pollers, release the awake lock. With PP_TOUCH_WAKE_ON_BATTERY (off under D1, "PWR only") the touch
// wake is armed first, and the function returns false, with the lock kept and the pollers as they were, if that cannot be done (no
// pin lock, the touch controller is not up): without a wake source the dial could not be woken by a touch, so it behaves as in S1
// until a later pass succeeds. Without it, the PWR key (polled by the ptt task on its timer wake) is the only wake source.
static bool s2_go_live(void)
{
    if (holds_now()) return false;               // work in flight (a voice turn, a link coming up): the lock stays; the policy moves to S1 at its next poll
#if PP_TOUCH_WAKE_ON_BATTERY
    if (!touch_sleep_arm()) {                    // INT (GPIO11) becomes the LOW-level wake source
        if (!s_touch_warned) ESP_LOGW(TAG, "touch wake not armed (no pin lock or controller not ready): the awake lock is kept");
        s_touch_warned = true;
        return false;
    }
#endif
    atomic_store(&s_relaxed, true);              // the pollers go to their S2 rates (a wake during the arm, or during the release below, clears the flag again in the check that follows)
    lock_release_once();                         // the chip may sleep from here
    // Look again after the release: the screen may have woken (its hook ran before the release took effect), or the ptt task may have
    // sent the policy back to S0 or S1 (a USB host, a plug-in, work started) while this ran. Either way take the lock back
    // (idempotent). s_in_s2 stays set so the next render pass runs exit_s2 and undoes the rest.
    if (!display_is_asleep() || (pp_state_t)atomic_load(&s_want) != PP_S2) {
        lock_acquire_once();
        atomic_store(&s_relaxed, false);
    }
    return true;
}

static void enter_s2(void)
{
    atomic_store(&s_in_s2, true);                // from here the screen hook takes the lock back on a wake
    s_audio_idle = audio_capture_idle(true);     // the I2S clocks stop; refused (and retried) while a beep plays
    display_render_guard(false);                 // the 1 s stall-guard timer would cap every sleep at one second
    s_s2_live = s2_go_live();
}

static void exit_s2(void)
{
    lock_acquire_once();                         // FIRST, and idempotent: the ptt task or the screen hook may have done it
    atomic_store(&s_relaxed, false);
    s_s2_live = false;
#if PP_TOUCH_WAKE_ON_BATTERY
    touch_sleep_disarm();                        // the pin goes back to the touch driver's falling-edge interrupt
#endif
    if (s_audio_idle) audio_capture_idle(false);
    s_audio_idle = false;
    display_render_guard(true);
    atomic_store(&s_in_s2, false);
}

void power_sleep_render_service(void)
{
    const pp_state_t want = (pp_state_t)atomic_load(&s_want);
    pp_state_t have = s_applied;
    if (want == have && have != PP_S2) return;
    if (have == PP_S2 && (want != PP_S2 || !display_is_asleep())) {
        exit_s2();
        have = PP_S1;
    }
    if (want == PP_S0 && have != PP_S0) {
        leave_battery();
        have = PP_S0;
    } else if (want != PP_S0 && have == PP_S0) {
        enter_battery();
        have = PP_S1;
    }
    if (have == PP_S1 && want == PP_S2 && display_is_asleep()) {
        enter_s2();
        have = PP_S2;
    } else if (have == PP_S2) {
        if (!s_audio_idle) s_audio_idle = audio_capture_idle(true);   // a beep was playing when S2 began: try again
        if (want == PP_S2 && display_is_asleep() && (!s_s2_live || (atomic_load(&s_lock_held) && !holds_now()))) {
            // Not live yet (the touch wake could not be armed, or a hold was on when S2 began), or a hold came and went between two ptt polls
            // and left the lock taken (on_hold): go live again.
            s_s2_live = s2_go_live();
#if PP_TOUCH_WAKE_ON_BATTERY
        } else if (s_s2_live && !touch_sleep_armed()) {
            // The touch driver was deleted (read errors) and not yet reopened: no wake source. Keep the dial up until it is armed again.
            ESP_LOGW(TAG, "touch wake lost: the awake lock is back until the controller is up again");
            lock_acquire_once();
            atomic_store(&s_relaxed, false);
            s_s2_live = false;
#endif
        }
    }
    if (have != s_applied) ESP_LOGI(TAG, "applied %s -> %s", pp_state_name(s_applied), pp_state_name(have));
    s_applied = have;
}

// ── the screen policy ───────────────────────────────────────────────────────────────────────────────

pp_screen_t power_sleep_screen_level(uint32_t idle_ms)
{
    // "On the battery" is the policy's own call (two fresh reads, no host): S1 or S2. A dial the policy cannot confirm keeps 300 s.
    return pp_screen_level((pp_state_t)atomic_load_explicit(&s_want, memory_order_relaxed), idle_ms);
}

bool power_sleep_input_allowed(void)
{
    // The policy's own state, as for the screen levels. Between a plug-in and the PMIC read that notices it (2 s at most) a dark screen
    // on a charger still ignores touch; the PWR key works the whole time.
    return pp_screen_input_allowed((pp_state_t)atomic_load_explicit(&s_want, memory_order_relaxed), display_is_asleep());
}

// ── the pollers ─────────────────────────────────────────────────────────────────────────────────────

uint32_t power_sleep_wait_ms(pp_poller_t who, uint32_t awake_ms)
{
    return pp_wait_ms(atomic_load_explicit(&s_relaxed, memory_order_relaxed) ? PP_S2 : PP_S1, who, awake_ms);
}

uint32_t power_sleep_ptt_poll_ms(uint32_t awake_ms)
{
    if (!atomic_load_explicit(&s_relaxed, memory_order_relaxed)) return awake_ms;
    const uint32_t relaxed = pp_ptt_poll_ms(PP_S2);
    return awake_ms > relaxed ? awake_ms : relaxed;
}

bool power_sleep_relaxed(void) { return atomic_load_explicit(&s_relaxed, memory_order_relaxed); }

// ── for the alive line ──────────────────────────────────────────────────────────────────────────────

const char *power_sleep_state_name(void) { return pp_state_name(s_applied); }
uint32_t power_sleep_sleeps(void) { return s_slept_count; }
uint32_t power_sleep_slept_ms(void) { return s_slept_ms; }
