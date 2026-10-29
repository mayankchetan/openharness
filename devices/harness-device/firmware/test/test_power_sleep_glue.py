"""The ESP-IDF half of battery sleep (main/power_sleep.c), run on the host against fake IDF headers.

The production power_sleep.c and power_policy.c are compiled unchanged, with a fake clock and fakes for the power-management locks,
esp_pm_configure, the PMIC, the USB host, the screen, the I2S clocks, the render stall guard and the touch wake. Every fake appends to
one event log, so the ORDER of the calls is checked, not only that they happened.

Checked here (plan sections 3.1, 3.2, 3.5 and tasks T6, T7):
  * boot: the awake lock is made and taken, the output pads are kept, nothing is configured, the policy is in S0;
  * unplug: two PMIC reads 2 s apart, then power management is configured for battery (240/80 MHz, light sleep on) once, with the
    retention RAM allocated first;
  * screen off for 3 s: the I2S clocks stop, the stall guard stops, the touch wake is armed, THEN the awake lock is released;
  * a wake: the awake lock is taken back by the screen hook BEFORE anything else, then the render task disarms the touch wake;
  * plug in while asleep: noticed on the 2 s read, lock back first, power management back to 240/240;
  * 200 plug/unplug cycles: the lock count is balanced, arm/disarm and stop/start are paired, configure calls are paired;
  * no retention RAM: frequency scaling only, the dial still works;
  * the screen waking in the middle of the S2 entry never leaves the chip free to sleep;
  * a dial that is not the CST9217 + AXP2101 board never leaves S0 and never reads the PMIC;
  * a voice turn or a cable session holds the dial in S1; a USB host sends it to S0 at once;
  * the sleep statistics of the alive line (count and milliseconds) from the light sleep exit callback.
"""
from pathlib import Path
import os
import subprocess
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'

FAKES = {
'sdkconfig.h': '''
#pragma once
#define CONFIG_PM_ENABLE 1
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 1
#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 1
''',
'esp_attr.h': '#pragma once\n#define IRAM_ATTR\n',
'fake_idf.h': '''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "OK" : e == ESP_ERR_NO_MEM ? "NO_MEM" : "ERR"; }
// the event log: every fake that matters appends one token
extern char g_events[8192];
extern int g_log_lines;
static inline void ev(const char *s) { size_t n = strlen(g_events); if (n + strlen(s) + 2 < sizeof g_events) { strcat(g_events, s); strcat(g_events, " "); } }
// the arguments are evaluated and format-checked, as in IDF; the line itself is only counted
#define ESP_LOGI(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__), g_log_lines++)
#define ESP_LOGW(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__), g_log_lines++)
#define ESP_LOGE(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__), g_log_lines++)
''',
'esp_log.h': '#pragma once\n#include "fake_idf.h"\n',
'esp_heap_caps.h': '''
#pragma once
#include "fake_idf.h"
#define MALLOC_CAP_INTERNAL 1
extern size_t g_int_free, g_largest;
static inline size_t heap_caps_get_free_size(int c) { (void)c; return g_int_free; }
static inline size_t heap_caps_get_largest_free_block(int c) { (void)c; return g_largest; }
''',
'esp_timer.h': '''
#pragma once
#include "fake_idf.h"
extern int64_t g_now_us;
static inline int64_t esp_timer_get_time(void) { return g_now_us; }
''',
'esp_pm.h': '''
#pragma once
#include "fake_idf.h"
typedef struct fake_lock *esp_pm_lock_handle_t;
typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP } esp_pm_lock_type_t;
typedef struct { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef esp_err_t (*esp_pm_light_sleep_cb_t)(int64_t sleep_time_us, void *arg);
typedef struct { esp_pm_light_sleep_cb_t enter_cb, exit_cb; void *enter_cb_user_arg, *exit_cb_user_arg; uint32_t enter_cb_prior, exit_cb_prior; } esp_pm_sleep_cbs_register_config_t;
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t t, int arg, const char *name, esp_pm_lock_handle_t *out);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h);
esp_err_t esp_pm_configure(const void *cfg);
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs_conf);
''',
'esp_sleep.h': '''
#pragma once
#include "fake_idf.h"
#define ESP_SLEEP_POWER_DOWN_CPU 1
esp_err_t esp_sleep_cpu_retention_init(void);
''',
'driver/gpio.h': '''
#pragma once
#include "../fake_idf.h"
typedef int gpio_num_t;
esp_err_t gpio_sleep_sel_dis(gpio_num_t g);
''',
'driver/usb_serial_jtag.h': '#pragma once\n#include "../fake_idf.h"\nbool usb_serial_jtag_is_connected(void);\n',
'board/board.h': '''
#pragma once
#include "../fake_idf.h"
typedef enum { TOUCH_NONE = 0, TOUCH_CST9217, TOUCH_CST816S, TOUCH_GT911 } touch_chip_t;
typedef struct { const char *name; int lcd_rst; int touch_rst; touch_chip_t touch; bool touch_mirror; bool has_pmic; } board_t;
const board_t *board(void);
''',
'board/power.h': '#pragma once\nint power_charge_state(void);\n',
'battery.h': '#pragma once\nint battery_chg(void);\nvoid battery_set(int pct, int chg);\nint battery_pct_raw(void);\n',
'audio_capture.h': '#pragma once\n#include <stdbool.h>\nbool audio_capture_idle(bool idle);\n',
'audio_client.h': '#pragma once\n#include <stdbool.h>\nbool audio_client_active(void);\n',
'cable_client.h': '#pragma once\n#include <stdbool.h>\nbool cable_client_is_connected(void);\n',
'fw_update.h': '#pragma once\n#include <stdbool.h>\nbool fw_update_active(void);\n',
'ui/display.h': '''
#pragma once
#include <stdbool.h>
bool display_is_asleep(void);
void display_set_sleep_cb(void (*cb)(bool on));
void display_render_guard(bool run);
''',
'ui/habitat/runtime.h': '#pragma once\nvoid habitat_render_notify(void);\n',
'ui/touch.h': '''
#pragma once
void touch_sleep_arm(void);
void touch_sleep_disarm(void);
''',
}

HARNESS = r'''
#include "fake_idf.h"
#include "power_sleep.h"
#include "power_policy.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "board/board.h"
#include <assert.h>

char g_events[8192];
int g_log_lines;
size_t g_int_free = 150000, g_largest = 100000;
int64_t g_now_us;

// ---- fake world ----
static struct fake_lock { int count; const char *name; } the_lock;
static int lock_creates, lock_acquires, lock_releases;
static int configure_calls, configure_battery, configure_usb;
static esp_pm_config_t last_cfg;
static esp_err_t retention_result = ESP_OK;
static int pads_kept;
static board_t the_board = {"cst9217+axp2101", 39, 40, TOUCH_CST9217, true, true};
static int pmic_state = 0;            // what the PMIC answers: 0 standby, 1 charging, 2 discharging, -1 error
static int cache_chg = -1, cache_pct = 55, pmic_reads;
static bool usb_host, asleep, voice, session, audio_idle_ok = true, audio_stopped;
static void (*sleep_cb)(bool);
static esp_pm_light_sleep_cb_t exit_cb;
static int notifies, guard_stops, guard_starts, audio_idles, audio_resumes, touch_arms, touch_disarms;
static bool arm_wakes_screen;         // the screen wakes in the middle of the S2 entry

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t t, int arg, const char *name, esp_pm_lock_handle_t *out) { assert(t == ESP_PM_NO_LIGHT_SLEEP && arg == 0); lock_creates++; the_lock.name = name; *out = &the_lock; return ESP_OK; }
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) { assert(h == &the_lock); h->count++; lock_acquires++; ev("LOCK+"); return ESP_OK; }
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) { assert(h == &the_lock && h->count > 0); h->count--; lock_releases++; ev("LOCK-"); return ESP_OK; }
esp_err_t esp_pm_configure(const void *cfg) {
    last_cfg = *(const esp_pm_config_t *)cfg; configure_calls++;
    if (last_cfg.min_freq_mhz == 240) { configure_usb++; ev("PM240"); } else { configure_battery++; ev(last_cfg.light_sleep_enable ? "PM80L" : "PM80"); }
    return ESP_OK;
}
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *c) { exit_cb = c->exit_cb; return ESP_OK; }
esp_err_t esp_sleep_cpu_retention_init(void) { ev("RET"); return retention_result; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t g) { (void)g; pads_kept++; return ESP_OK; }
bool usb_serial_jtag_is_connected(void) { return usb_host; }
const board_t *board(void) { return &the_board; }
int power_charge_state(void) { pmic_reads++; return pmic_state; }
int battery_chg(void) { return cache_chg; }
void battery_set(int pct, int chg) { cache_pct = pct; cache_chg = chg; }
int battery_pct_raw(void) { return cache_pct; }
bool audio_capture_idle(bool idle) { if (idle) { audio_idles++; if (audio_idle_ok) { audio_stopped = true; ev("AUDIO-"); } return audio_idle_ok; } audio_resumes++; audio_stopped = false; ev("AUDIO+"); return true; }
bool audio_client_active(void) { return voice; }
bool cable_client_is_connected(void) { return session; }
bool fw_update_active(void) { return false; }
bool display_is_asleep(void) { return asleep; }
void display_set_sleep_cb(void (*cb)(bool)) { sleep_cb = cb; }
void display_render_guard(bool run) { if (run) { guard_starts++; ev("GUARD+"); } else { guard_stops++; ev("GUARD-"); } }
void habitat_render_notify(void) { notifies++; }
static void screen(bool on_);
void touch_sleep_arm(void) {
    touch_arms++; ev("TARM");
    if (arm_wakes_screen) { arm_wakes_screen = false; screen(true); }   // a finger lands while the wake is being armed
}
void touch_sleep_disarm(void) { touch_disarms++; ev("TDIS"); }

// ---- helpers ----
static void clear_events(void) { g_events[0] = 0; }
static bool in_order(const char *a, const char *b) {   // a occurs, and b occurs after it
    const char *pa = strstr(g_events, a);
    return pa && strstr(pa + strlen(a), b);
}
// the ptt task and the render task, 100 ms apart (the policy asks for 250 ms in S2; both are fine for the logic)
static void tick(int ms) {
    for (int t = 0; t < ms; t += 100) {
        g_now_us += 100000;
        power_sleep_poll();
        power_sleep_render_service();
    }
}
static void screen(bool on_) {   // display_sleep() / display_wake(): flag first, then the hook (display_habitat.c order)
    asleep = !on_;
    if (sleep_cb) sleep_cb(on_);
}
static void plug_in(void)  { pmic_state = 1; cache_chg = 1; }
static void unplug(void)   { pmic_state = 2; cache_chg = 2; }
static void reset_world(void) {
    memset(&the_lock, 0, sizeof the_lock); lock_creates = lock_acquires = lock_releases = 0;
    configure_calls = configure_battery = configure_usb = 0; pads_kept = 0; pmic_reads = 0;
    cache_chg = -1; usb_host = asleep = voice = session = false; audio_stopped = false; audio_idle_ok = true;
    guard_stops = guard_starts = audio_idles = audio_resumes = touch_arms = touch_disarms = 0; pmic_state = 0; retention_result = ESP_OK;
    arm_wakes_screen = false;
    clear_events();
}

// Each scenario runs in its own process: power_sleep_init() is called once per boot, and its statics are the boot state.
static void scenario_boot_and_cycle(void) {
    // ---------- boot ----------
    reset_world();
    power_sleep_init();
    assert(lock_creates == 1 && the_lock.count == 1 && !strcmp(the_lock.name, "dial_awake"));
    assert(pads_kept == 9 && sleep_cb && exit_cb);         // CS, SCLK, D0-D3, LCD reset, touch reset, amplifier enable
    assert(configure_calls == 0);                          // S0 never calls esp_pm_configure
    assert(!power_sleep_relaxed() && !strcmp(power_sleep_state_name(), "S0"));
    assert(power_sleep_wait_ms(PP_POLLER_RENDER, 1000) == 1000 && power_sleep_ptt_poll_ms(100) == 100);
    // on a charger nothing happens, for a long time, and the PMIC is not read by the policy at all
    cache_chg = 1; pmic_state = 1;
    tick(60000);
    assert(configure_calls == 0 && pmic_reads == 0 && the_lock.count == 1);

    // ---------- unplug: S0 -> S1 ----------
    unplug();
    clear_events();
    tick(1900);
    assert(configure_calls == 0 && pmic_reads == 1);       // one fresh read so far, not enough
    tick(300);
    assert(pmic_reads == 2 && configure_battery == 1 && configure_usb == 0);
    assert(last_cfg.max_freq_mhz == 240 && last_cfg.min_freq_mhz == 80);
    assert(last_cfg.light_sleep_enable);                   // light sleep on
    assert(in_order("RET", "PM80L"));                      // the retention RAM is allocated first and looked at
    assert(the_lock.count == 1 && !power_sleep_relaxed() && !strcmp(power_sleep_state_name(), "S1"));

    // ---------- screen off for 3 s: S1 -> S2 ----------
    clear_events();
    screen(false);
    tick(2800);
    assert(!power_sleep_relaxed() && guard_stops == 0 && touch_arms == 0 && the_lock.count == 1);   // 2.8 s: not yet
    tick(500);
    assert(power_sleep_relaxed() && !strcmp(power_sleep_state_name(), "S2"));
    assert(guard_stops == 1 && audio_stopped && touch_arms == 1);
    assert(power_sleep_wait_ms(PP_POLLER_TOUCH, 20) == 1000 && power_sleep_wait_ms(PP_POLLER_RENDER, 1000) == 5000 && power_sleep_ptt_poll_ms(100) == 250);
    assert(the_lock.count == 0);                           // the chip may sleep
    assert(in_order("AUDIO-", "TARM") && in_order("GUARD-", "TARM") && in_order("TARM", "LOCK-"));   // arm the wake, THEN let go of the lock

    // ---------- a wake: the lock first ----------
    clear_events();
    screen(true);                                           // the hook runs inside display_wake()
    assert(the_lock.count == 1 && !power_sleep_relaxed());  // before the render task has run: lock back, pollers back to their S1 rates
    assert(power_sleep_wait_ms(PP_POLLER_TOUCH, 20) == 20);
    assert(strncmp(g_events, "LOCK+", 5) == 0 && touch_disarms == 0);
    tick(300);
    assert(touch_disarms == 1 && guard_starts == 1 && audio_resumes == 1 && !audio_stopped);
    assert(in_order("LOCK+", "TDIS") && in_order("TDIS", "AUDIO+") && in_order("AUDIO+", "GUARD+"));
    assert(the_lock.count == 1);

    // ---------- plug in while asleep ----------
    screen(false);
    tick(4000);
    assert(power_sleep_relaxed() && the_lock.count == 0);
    plug_in();
    clear_events();
    {
        int waited = 0;
        while (power_sleep_relaxed() && waited < 3000) { tick(100); waited += 100; }
        assert(waited <= 2200);                             // noticed on the 2 s PMIC read (+ one poll)
    }
    assert(the_lock.count == 1 && g_events[0] == 'L');      // the ptt task took the lock back first, in the same step it noticed
    tick(500);
    assert(configure_usb == 1 && last_cfg.max_freq_mhz == 240 && last_cfg.min_freq_mhz == 240 && !last_cfg.light_sleep_enable);
    assert(guard_starts == 2 && touch_disarms == 2 && !audio_stopped);
    assert(in_order("LOCK+", "TDIS") && in_order("TDIS", "GUARD+") && in_order("GUARD+", "PM240"));   // lock, pin back to the driver, guard, then the clocks
    assert(!strcmp(power_sleep_state_name(), "S0"));
}

static void scenario_cycles(void) {
    // ---------- 200 plug/unplug cycles ----------
    reset_world();
    power_sleep_init();
    for (int i = 0; i < 200; i++) {
        unplug();
        tick(2500 + (i % 7) * 100);                         // S1
        if (i % 2) { screen(false); tick(4000); }           // half of the cycles reach S2
        if (i % 3 == 0) { screen(true); tick(300); }        // some wake first
        plug_in();
        tick(2300);                                         // back to S0
        screen(true);
        tick(10500);                                        // out the anti-flap dwell
    }
    if (the_lock.count != 1) printf("lock count %d, acquires %d, releases %d, configure %d/%d, tail of events: %s\n", the_lock.count, lock_acquires, lock_releases, configure_battery, configure_usb, g_events + (strlen(g_events) > 200 ? strlen(g_events) - 200 : 0));
    assert(the_lock.count == 1);                            // balanced
    assert(configure_battery == configure_usb);             // paired
    assert(configure_battery > 100);
    assert(guard_stops == guard_starts);                    // the stall guard is stopped and started in pairs
    assert(touch_arms == touch_disarms && touch_arms > 50); // the wake is armed and put back in pairs
    assert(audio_resumes >= touch_arms && !audio_stopped);  // (audio_resumes also counts nothing else)
    assert(!power_sleep_relaxed());
}

static void scenario_no_retention(void) {
    reset_world();
    power_sleep_init();
    retention_result = ESP_ERR_NO_MEM;
    unplug(); tick(4000);
    assert(configure_battery == 1 && !last_cfg.light_sleep_enable && last_cfg.min_freq_mhz == 80);   // frequency scaling only
    assert(in_order("RET", "PM80") && !strstr(g_events, "PM80L"));
    screen(false); tick(4000);
    assert(power_sleep_relaxed());                          // the rest of S2 still works
    screen(true); tick(300);
    assert(!power_sleep_relaxed() && the_lock.count == 1);
}

static void scenario_wake_during_entry(void) {
    reset_world();
    power_sleep_init();
    unplug(); tick(4000);
    screen(false);
    arm_wakes_screen = true;
    tick(3300);                                             // the finger lands while the wake is being armed
    assert(the_lock.count == 1);                            // the chip is NOT left free to sleep under the finger
    assert(!power_sleep_relaxed());
    tick(500);
    assert(touch_arms == touch_disarms && the_lock.count == 1 && !power_sleep_relaxed());
    assert(guard_stops == guard_starts && !audio_stopped);
}

static void scenario_other_boards(void) {
    // ---------- other boards never leave S0 ----------
    reset_world();
    power_sleep_init();
    the_board.touch = TOUCH_CST816S;
    unplug(); tick(30000);
    assert(configure_calls == 0 && pmic_reads == 0);
    the_board.touch = TOUCH_CST9217; the_board.has_pmic = false;
    tick(30000);
    assert(configure_calls == 0 && pmic_reads == 0);
    the_board.has_pmic = true;
}

static void scenario_usb_host(void) {
    // ---------- a USB host keeps the dial in S0, and sends it back to S0 ----------
    reset_world();
    power_sleep_init();
    unplug(); usb_host = true; tick(20000);
    assert(configure_calls == 0);
    usb_host = false; tick(4000);
    assert(configure_battery == 1);
    screen(false); tick(4000);
    assert(power_sleep_relaxed() && the_lock.count == 0);
    usb_host = true; tick(100);
    assert(!power_sleep_relaxed() && the_lock.count == 1);  // at once, on the ptt task
    tick(300);
    assert(configure_usb == 1 && the_lock.count == 1 && touch_disarms == 1);
}

static void scenario_holds(void) {
    // ---------- work in flight holds S1 ----------
    reset_world();
    power_sleep_init();
    unplug(); tick(4000);
    voice = true; screen(false); tick(10000);
    assert(!power_sleep_relaxed() && the_lock.count == 1);
    voice = false; tick(500);
    assert(power_sleep_relaxed() && the_lock.count == 0);
    session = true; tick(500);
    assert(!power_sleep_relaxed() && the_lock.count == 1);
    session = false; power_sleep_hold(PP_HOLD_LINK); tick(5000);
    assert(!power_sleep_relaxed());
    power_sleep_release(PP_HOLD_LINK); tick(3500);
    assert(power_sleep_relaxed());
}

static void scenario_audio_retry(void) {
    // ---------- an audio stop that was refused (a beep) is retried ----------
    reset_world();
    power_sleep_init();
    unplug(); tick(4000);
    audio_idle_ok = false;
    screen(false); tick(4000);
    assert(power_sleep_relaxed() && !audio_stopped);
    audio_idle_ok = true; tick(300);
    assert(audio_stopped);
}

static void scenario_stats(void) {
    reset_world();
    power_sleep_init();
    assert(power_sleep_sleeps() == 0 && power_sleep_slept_ms() == 0);
    assert(exit_cb(5000, NULL) == ESP_OK && exit_cb(5000, NULL) == ESP_OK && exit_cb(2500000, NULL) == ESP_OK);
    assert(power_sleep_sleeps() == 3 && power_sleep_slept_ms() == 2510);
    exit_cb(0, NULL); exit_cb(300, NULL); exit_cb(999, NULL);   // a rejected sleep returns at once: not counted
    assert(power_sleep_sleeps() == 3 && power_sleep_slept_ms() == 2510);
    for (int i = 0; i < 1000; i++) exit_cb(1500, NULL);         // the microsecond remainder carries: 1000 x 1.5 ms
    assert(power_sleep_sleeps() == 1003 && power_sleep_slept_ms() == 2510 + 1500);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "boot")) scenario_boot_and_cycle();
    else if (!strcmp(argv[1], "cycles")) scenario_cycles();
    else if (!strcmp(argv[1], "noret")) scenario_no_retention();
    else if (!strcmp(argv[1], "race")) scenario_wake_during_entry();
    else if (!strcmp(argv[1], "boards")) scenario_other_boards();
    else if (!strcmp(argv[1], "host")) scenario_usb_host();
    else if (!strcmp(argv[1], "holds")) scenario_holds();
    else if (!strcmp(argv[1], "audio")) scenario_audio_retry();
    else if (!strcmp(argv[1], "stats")) scenario_stats();
    else assert(!"unknown scenario");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='harness-power-sleep-') as folder:
    folder = Path(folder)
    for name, text in FAKES.items():
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    # the production sources are compiled from COPIES placed beside the fake headers: a quoted include looks in the including
    # file's own directory first, so the fakes win over the real headers of main/ (power_sleep.h, power_policy.h, sleep_pins.h
    # are not faked and come from main/ through -I)
    for name in ('power_sleep.c', 'power_policy.c'):
        (folder / name).write_text((main / name).read_text())
    (folder / 'harness.c').write_text(HARNESS)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g', '-DDEVICE_BATTERY_SLEEP=1', '-DDEVICE_NO_OTA=1',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(main), str(folder / 'harness.c'), str(folder / 'power_sleep.c'), str(folder / 'power_policy.c'),
                    '-o', str(folder / 'glue')], check=True)
    scenarios = ('boot', 'cycles', 'noret', 'race', 'boards', 'host', 'holds', 'audio', 'stats')
    for name in scenarios:
        subprocess.run([str(folder / 'glue'), name], check=True)
    print('power sleep glue: ' + ', '.join(scenarios) + ' PASS (boot order, unplug, S2 entry and wake order, plug-in, 200 cycles balanced, '
          'no retention RAM, wake during entry, other boards, host, holds, audio retry, sleep statistics)')
