"""The touch wake of battery sleep (touch_habitat.c, the block between 'BEGIN battery sleep (touch)' and 'END ...'), run on the host.

The production functions are cut out of the source and run against fake GPIO, sleep and FreeRTOS calls with an event log. Checked:
  * arm: the flag is set BEFORE the pin becomes a level wake source (so the first level hit is already handled), then the GPIO wake
    source, then the interrupt; arming twice does nothing; a dial without the CST9217 is left alone;
  * touch_irq: armed, the first hit switches the interrupt off and notes it, unarmed it only notifies the task (today's behaviour);
  * disarm: interrupt off, wake off, wake source off, the driver's falling-edge type back, flag cleared, interrupt on, in that order;
    disarming twice does nothing;
  * a sample without a contact after an INT hit: up to 3 more reads 5 ms apart, then the interrupt goes back on when INT is high;
  * INT stuck low with nothing to read: the wake is parked (chip may sleep), and armed again by the first poll that finds INT high;
  * unarmed, the touch task helper does nothing at all.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / '../main/ui/habitat/touch_habitat.c').read_text()
block = re.search(r'// BEGIN battery sleep \(touch\)\n(.*?)// END battery sleep \(touch\)', source, re.S)
assert block, 'sleep block markers'
body = '\n'.join(line for line in block[1].split('\n') if not line.startswith('#include'))
irq = re.search(r'static void touch_irq\(esp_lcd_touch_handle_t tp\)\n\{.*?^\}', source, re.M | re.S)[0]
wait = re.search(r'static TickType_t touch_wait_ticks\(bool down\)\n\{.*?^\}', source, re.M | re.S)[0]
# a driver that is deleted and opened again re-arms the pin (open_touch), and the flag-off build has empty stand-ins
assert re.search(r'atomic_store\(&controller_ready, true\);\n    touch_sleep_reopened\(\);', source)
assert 'static inline bool touch_sleep_parked(void) { return false; }' in block[1] and 'static inline void touch_sleep_reopened(void) {}' in block[1]
assert 'touch_sleep_irq();' in irq
# the touch task calls the helper after every sample without a contact, and the pin lock is made in touch_init
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n        if \(!down && touch_sleep_unconfirmed\(\)\)[^\n]*\n            continue;\n#endif', source)
assert 's_pin_lock = xSemaphoreCreateMutex();' in re.search(r'void touch_init\(void\)\n\{.*?^\}', source, re.M | re.S)[0]
# flag off: the block is an empty inline and the image has none of it
assert re.search(r'#else\nstatic inline void touch_sleep_irq\(void\) \{\}\n(?:static inline [^\n]*\n)*#endif', block[1])
assert 'gpio_wakeup_enable' not in source.split('// END battery sleep (touch)')[1]

code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DEVICE_BATTERY_SLEEP 1
#define BSP_TOUCH_INT 11
#define GPIO_INTR_NEGEDGE 2
#define GPIO_INTR_LOW_LEVEL 4
#define ESP_SLEEP_WAKEUP_GPIO 7
#define pdMS_TO_TICKS(x) (x)
#define pdFALSE 0
#define portYIELD_FROM_ISR() (yields++)
typedef int esp_err_t; typedef int gpio_num_t; typedef int BaseType_t; typedef unsigned TickType_t;
static atomic_bool controller_ready = true;
typedef enum { PP_POLLER_TOUCH } pp_poller_t;
static bool relaxed_now;   /* the sleep policy's answer: the 1 s S2 poll when relaxed, 20 ms otherwise */
static unsigned power_sleep_wait_ms(pp_poller_t who, unsigned awake_ms) { (void)who; return relaxed_now && awake_ms < 1000 ? 1000 : awake_ms; }
static bool screen_asleep, input_ok = true;   /* display_is_asleep() and power_sleep_input_allowed() (D1: false on the battery with the screen off) */
static bool display_is_asleep(void) { return screen_asleep; }
static bool power_sleep_input_allowed(void) { return input_ok; }
typedef void *SemaphoreHandle_t; typedef void *esp_lcd_touch_handle_t; typedef void *TaskHandle_t;
typedef enum { TOUCH_NONE, TOUCH_CST9217, TOUCH_CST816S } touch_chip_t;
typedef struct { touch_chip_t touch; } board_t;
static board_t the_board = {TOUCH_CST9217};
static const board_t *board(void) { return &the_board; }
static char ev[512]; static void e(const char *s) { strcat(ev, s); strcat(ev, " "); }
static int yields, notifies, delays_ms, lock_depth, int_level = 1;
static int last_wake_type, last_intr_type;
static TaskHandle_t touch_task_handle = (void *)1;
static void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; (void)w; notifies++; }
static void vTaskDelay(int ms) { delays_ms += ms; e("DELAY"); }
static SemaphoreHandle_t s_pin_lock_made;
static int xSemaphoreTake(SemaphoreHandle_t l, int t) { (void)l; (void)t; assert(lock_depth == 0); lock_depth++; return 1; }
static void xSemaphoreGive(SemaphoreHandle_t l) { (void)l; assert(lock_depth == 1); lock_depth--; }
#define portMAX_DELAY 0
static atomic_bool *flag_probe;   // set below: lets the fakes look at s_level_wake at the moment of a call
static esp_err_t gpio_intr_disable(gpio_num_t g) { assert(g == 11); e("IDIS"); return 0; }
static esp_err_t gpio_intr_enable(gpio_num_t g) { assert(g == 11); e("IEN"); return 0; }
static esp_err_t gpio_set_intr_type(gpio_num_t g, int t) { assert(g == 11); last_intr_type = t; e(t == GPIO_INTR_NEGEDGE ? "TYPE-EDGE" : "TYPE?"); return 0; }
static int gpio_get_level(gpio_num_t g) { assert(g == 11); return int_level; }
static esp_err_t esp_sleep_enable_gpio_wakeup(void) { e("GPWAKE+"); return 0; }
static esp_err_t esp_sleep_disable_wakeup_source(int s) { assert(s == ESP_SLEEP_WAKEUP_GPIO); e("GPWAKE-"); return 0; }
static esp_err_t gpio_wakeup_enable(gpio_num_t g, int t);
static esp_err_t gpio_wakeup_disable(gpio_num_t g) { assert(g == 11); e("WDIS"); return 0; }
static int64_t fake_now_us = 777000;
static int64_t esp_timer_get_time(void) { return fake_now_us; }

''' + body + irq + wait + r'''
static esp_err_t gpio_wakeup_enable(gpio_num_t g, int t) {
    assert(g == 11 && t == GPIO_INTR_LOW_LEVEL); last_wake_type = t;
    e(atomic_load(&s_level_wake) ? "WEN(flag-set)" : "WEN(flag-CLEAR)"); return 0;
}
static void reset(void) { ev[0] = 0; yields = notifies = delays_ms = 0; int_level = 1; the_board.touch = TOUCH_CST9217; }
int main(void) {
    s_pin_lock = &s_pin_lock_made;
    // --- unarmed: the ISR only notifies; the task helper does nothing ---
    reset();
    touch_irq(NULL);
    assert(notifies == 1 && !ev[0] && !atomic_load(&s_irq_hit));
    assert(!touch_sleep_unconfirmed() && !ev[0]);
    touch_sleep_disarm();
    assert(!ev[0]);                                        // disarming what was never armed touches nothing
    // --- arm ---
    reset();
    assert(!touch_sleep_armed());
    assert(touch_sleep_arm() && touch_sleep_armed());
    assert(!strcmp(ev, "WEN(flag-set) GPWAKE+ IEN "));     // the flag was already set when the pin became a level source
    assert(atomic_load(&s_level_wake) && !atomic_load(&s_irq_hit) && !atomic_load(&s_parked));
    ev[0] = 0; assert(touch_sleep_arm()); assert(!ev[0]);  // twice is once, and still "armed"
    // --- the first level hit switches the interrupt off, and stamps the time for the wake-to-frame log ---
    assert(touch_sleep_take_isr_us() == 0);
    touch_irq(NULL);
    assert(!strcmp(ev, "IDIS ") && atomic_load(&s_irq_hit) && notifies == 1);
    fake_now_us = 779000; touch_irq(NULL);                  // a second hit keeps the first stamp
    assert(touch_sleep_take_isr_us() == 777000 && touch_sleep_take_isr_us() == 0);
    atomic_store(&s_isr_us, 5); touch_sleep_disarm(); touch_sleep_arm();
    assert(touch_sleep_take_isr_us() == 0);                 // arming forgets an old stamp
    touch_irq(NULL); ev[0] = 0;                              // the state after a first hit again
    // --- a sample without a contact: three more reads 5 ms apart, then the interrupt goes back on ---
    ev[0] = 0; delays_ms = 0;
    assert(touch_sleep_unconfirmed() && touch_sleep_unconfirmed() && touch_sleep_unconfirmed());
    assert(delays_ms == 15 && !strstr(ev, "IEN"));
    assert(!touch_sleep_unconfirmed());
    assert(strstr(ev, "IEN") && !atomic_load(&s_irq_hit) && !atomic_load(&s_parked));
    // a poll with nothing pending and INT high: the interrupt is simply enabled again (idempotent)
    ev[0] = 0;
    assert(!touch_sleep_unconfirmed());
    assert(!strcmp(ev, "IEN "));
    // --- INT stuck low: park the wake, then re-arm it on the poll that finds INT high ---
    reset();
    touch_sleep_disarm(); touch_sleep_arm(); ev[0] = 0;
    touch_irq(NULL); int_level = 0;
    for (int i = 0; i < 3; i++) assert(touch_sleep_unconfirmed());
    ev[0] = 0;
    assert(!touch_sleep_unconfirmed());
    assert(!strcmp(ev, "WDIS ") && atomic_load(&s_parked));   // chip may sleep; no more waking at once
    ev[0] = 0; assert(!touch_sleep_unconfirmed());
    assert(!ev[0]);                                            // still low: nothing to do
    int_level = 1;
    assert(!touch_sleep_unconfirmed());
    assert(!strcmp(ev, "WEN(flag-set) GPWAKE+ IEN ") && !atomic_load(&s_parked) && !atomic_load(&s_irq_hit));
    // --- disarm: the order that cannot storm ---
    ev[0] = 0;
    touch_sleep_disarm();
    assert(!strcmp(ev, "IDIS WDIS GPWAKE- TYPE-EDGE IEN ") && last_intr_type == GPIO_INTR_NEGEDGE);
    assert(!atomic_load(&s_level_wake) && !atomic_load(&s_irq_hit) && !atomic_load(&s_parked));
    ev[0] = 0; touch_sleep_disarm(); assert(!ev[0]);          // twice is once
    // after the disarm the ISR leaves the interrupt alone again
    ev[0] = 0; touch_irq(NULL); assert(!ev[0]);
    // --- a dial without the CST9217 is left alone ---
    reset(); the_board.touch = TOUCH_CST816S;
    assert(!touch_sleep_arm() && !ev[0] && !atomic_load(&s_level_wake));
    // --- the controller is not up (its driver was deleted): nothing is armed and the answer says so ---
    reset(); atomic_store(&controller_ready, false);
    assert(!touch_sleep_arm() && !ev[0] && !atomic_load(&s_level_wake) && !touch_sleep_armed());
    atomic_store(&controller_ready, true);
    { SemaphoreHandle_t keep = s_pin_lock; s_pin_lock = NULL; assert(!touch_sleep_arm() && !ev[0]); s_pin_lock = keep; }   // no pin lock: no wake
    // --- armed, then the driver is deleted and reopened: armed() follows the controller, and the reopen arms the pin again ---
    reset();
    assert(touch_sleep_arm()); ev[0] = 0;
    atomic_store(&controller_ready, false); assert(!touch_sleep_armed());
    atomic_store(&controller_ready, true);
    atomic_store(&s_irq_hit, true); atomic_store(&s_parked, true);
    touch_sleep_reopened();
    assert(!strcmp(ev, "WEN(flag-set) GPWAKE+ IEN ") && touch_sleep_armed() && !atomic_load(&s_irq_hit) && !atomic_load(&s_parked));
    touch_sleep_disarm(); ev[0] = 0;
    touch_sleep_reopened(); assert(!ev[0]);                  // a reopen while unarmed changes nothing
    // --- the touch poll: 4 ms in a contact, 20 ms idle, the relaxed 1 s poll in S2, and 200 ms while the wake is parked ---
    reset();
    assert(touch_wait_ticks(true) == 4 && touch_wait_ticks(false) == 20);
    relaxed_now = true;
    assert(touch_wait_ticks(true) == 4 && touch_wait_ticks(false) == 1000);
    assert(touch_sleep_arm() && touch_wait_ticks(false) == 1000);
    atomic_store(&s_parked, true);
    assert(touch_wait_ticks(false) == 200 && touch_wait_ticks(true) == 4);
    atomic_store(&s_parked, false);
    relaxed_now = false; assert(touch_wait_ticks(false) == 20); touch_sleep_disarm();
    // --- a contact on a dark battery screen is dropped (D1), so a held contact is read at the relaxed rate, not every 4 ms (2026-11-02 review) ---
    reset();
    screen_asleep = true; input_ok = false; relaxed_now = true;                           /* S2: dark, input refused */
    assert(touch_wait_ticks(true) == 1000 && touch_wait_ticks(false) == 1000);            /* a held contact waits as long as an idle one */
    relaxed_now = false;                                                                  /* S1 in its 3 s settle: dark, input refused, not yet relaxed */
    assert(touch_wait_ticks(true) == 20 && touch_wait_ticks(false) == 20);
    input_ok = true; relaxed_now = true;                                                  /* external power: a dark screen takes touch, so a contact is read at 4 ms */
    assert(touch_wait_ticks(true) == 4);
    screen_asleep = false; input_ok = false;                                              /* the screen is on: input is allowed whatever the flag says; 4 ms */
    assert(touch_wait_ticks(true) == 4);
    screen_asleep = false; input_ok = true; relaxed_now = false;
    puts("touch sleep: arm order, a held contact on a dark battery screen is read at the idle rate, first-hit interrupt switch-off, 3 re-reads 5 ms apart, park and re-arm, disarm order, other chips PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-touch-sleep-') as folder:
    folder = Path(folder)
    (folder / 'sleep.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), str(folder / 'sleep.c'),
                    '-o', str(folder / 'sleep')], check=True)
    subprocess.run([str(folder / 'sleep')], check=True)
