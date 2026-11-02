"""mode_sw_task (mode_glue.c): every pass of the loop must wait, also when the USB probe is wanted but would not install.

2026-11-02 review finding: with the probe on and usb_probe_start() failing (no memory, the driver would not install), usb_probe_poll() returns at once
because the probe is not running, and the loop had no other wait. The task then spun, never let the idle task run, and tripped the idle task watchdog
(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y).

The production task is cut out of mode_glue.c and run against a fake clock, a fake probe and a fake task delay. Checked, for each of the three cases:
  * probe off:             one delay per pass (power_sleep_wait_ms(PP_POLLER_MODE_SW, IDLE_MS)), as before;
  * probe on and running:  the probe's own read is the wait (usb_probe_poll advances the clock by its wait), no extra delay, as before;
  * probe on, not running: usb_probe_start is tried again no more often than every PROBE_RETRY_MS, and every pass waits (PROBE_READ_MS awake, the relaxed
                           mode switch poll while the chip may sleep). Before the fix this case made 0 delays in 1,000 passes.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
source = (main / 'mode_glue.c').read_text()


def function(name):
    match = re.search(r'^static void ' + name + r'\(void \*arg\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


defines = ''.join(m.group(0) + '\n' for m in re.finditer(r'^#define (?:IDLE_MS|PROBE_READ_MS|PROBE_RETRY_MS)\b[^\n]*', source, re.M))
assert 'IDLE_MS' in defines and 'PROBE_READ_MS' in defines and 'PROBE_RETRY_MS' in defines

code = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mode_glue_core.h"
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
typedef enum { PP_POLLER_MODE_SW } pp_poller_t;
static const char *TAG = "mode_sw";
''' + defines + r'''
static uint64_t clock_ms;
static uint64_t uptime_ms(void) { return clock_ms; }
static mg_t s_mg;
static atomic_bool s_reload, s_keep_wifi;
static bool s_crash_usb;
static unsigned s_crashes;
static const mg_ops_t OPS;
static bool relaxed;                                  /* the chip may sleep: the mode switch poll is 2 s */
static bool probe_running, probe_start_ok;
static unsigned delays, delay_ms_total, starts, polls, passes, steps, spins;
static jmp_buf done;
static unsigned power_sleep_wait_ms(pp_poller_t who, unsigned awake_ms) { (void)who; return relaxed && awake_ms < 2000 ? 2000 : awake_ms; }
static void vTaskDelay(unsigned ticks) { delays++; delay_ms_total += ticks; clock_ms += ticks; if (++passes >= 1000) longjmp(done, 1); }
static unsigned uxTaskGetStackHighWaterMark(void *t) { (void)t; return 2000; }
static uint8_t config_load_wl_effective(void) { return 1; }
static bool config_load_stay_wl(void) { return false; }
void mg_apply_settings(mg_t *g, uint8_t wl, bool stay, uint64_t now) { (void)g; (void)wl; (void)stay; (void)now; }
void mg_step(mg_t *g, const mg_world_t *w, const mg_ops_t *ops) { (void)g; (void)w; (void)ops; steps++; }
static void sample(mg_world_t *w, uint64_t now) { memset(w, 0, sizeof *w); w->now_ms = now; }
static bool usb_probe_running(void) { return probe_running; }
static bool usb_probe_start(uint64_t now) { (void)now; starts++; if (probe_start_ok) probe_running = true; return probe_running; }
static void usb_probe_poll(uint64_t now, uint32_t wait_ms) {
    (void)now; polls++;
    if (!probe_running) { if (++spins >= 100000) longjmp(done, 1); return; }   /* the production code returns at once; 100,000 passes with no wait is a spin */
    clock_ms += wait_ms;                             /* a running probe reads for up to wait_ms: that is the pass's wait */
    if (++passes >= 1000) longjmp(done, 1);
}
''' + function('mode_sw_task') + r'''
static void run(bool probe_on, bool start_ok, bool relaxed_now) {
    memset(&s_mg, 0, sizeof s_mg);
    s_mg.probe_on = probe_on;
    probe_running = false; probe_start_ok = start_ok; relaxed = relaxed_now;
    clock_ms = 100000; delays = delay_ms_total = starts = polls = passes = steps = spins = 0;
    if (!setjmp(done)) mode_sw_task(NULL);
}
int main(void) {
    // probe off: one delay per pass, 250 ms awake, 2 s while the chip may sleep
    run(false, false, false); assert(delays == 1000 && delay_ms_total == 1000u * IDLE_MS && starts == 0);
    run(false, false, true);  assert(delays == 1000 && delay_ms_total == 1000u * 2000u && starts == 0);
    // probe on and installed: the probe's read is the wait; no delay of the task's own
    run(true, true, false);   assert(starts == 1 && delays == 0 && polls >= 1000);
    // probe on, would not install: it is tried again every PROBE_RETRY_MS and the task WAITS every pass
    run(true, false, false);
    assert(delays == 1000 && delay_ms_total == 1000u * PROBE_READ_MS);                   /* awake: PROBE_READ_MS per pass */
    assert(starts >= 1 && starts <= 1 + delay_ms_total / PROBE_RETRY_MS);                /* not more often than every PROBE_RETRY_MS */
    run(true, false, true);
    assert(delays == 1000 && delay_ms_total == 1000u * 2000u);                           /* while the chip may sleep: the relaxed 2 s poll */
    assert(starts >= 1 && starts <= 1 + delay_ms_total / PROBE_RETRY_MS);
    puts("mode switch task: a pass always waits (probe off, probe running, probe wanted but not installed), retry no faster than every 5 s PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-mode-sw-wait-') as folder:
    folder = Path(folder)
    (folder / 'w.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), '-I', str(main), str(folder / 'w.c'), '-o', str(folder / 'w')], check=True)
    subprocess.run([str(folder / 'w')], check=True)
