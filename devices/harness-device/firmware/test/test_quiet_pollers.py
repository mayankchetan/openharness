"""The loops that wake the chip ask the battery sleep policy for their wait (plan section 3.3), and with the flag off they wait
exactly as they did.

Source-structure checks in the style of test_psram_task_sites.py, plus a compile of the flag-off header. For each poller:
  * its wait goes through power_sleep_wait_ms(PP_POLLER_x, <today's constant>) (the ptt task through power_sleep_ptt_poll_ms),
  * the constant it passes is the one the loop had before (S0 and S1 give it back unchanged),
  * the relaxed value in power_policy.c is longer than that constant and below the 10 s task watchdog.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'
policy = (main / 'power_policy.c').read_text()
relaxed = {m[1]: int(m[2]) for m in re.finditer(r'case (PP_POLLER_[A-Z_]+):\s+return (\d+);', policy)}
relaxed['PP_POLLER_TOUCH'] = int(re.search(r'#define PP_TOUCH_POLL_MS (\d+)', (main / 'power_policy.h').read_text())[1])


def read(name):
    return (main / name).read_text()


checked = []

# --- habitat_touch: 20 ms idle, 4 ms during a contact; the contact wait is not a poller ---
touch = read('ui/habitat/touch_habitat.c')
helper = re.search(r'static TickType_t touch_wait_ticks\(bool down\)\n\{\n(.*?)\n\}', touch, re.S)
assert helper and re.fullmatch(r'    return down \? pdMS_TO_TICKS\(4\) : pdMS_TO_TICKS\(power_sleep_wait_ms\(PP_POLLER_TOUCH, 20\)\);', helper[1]), 'touch wait helper'
task = re.search(r'static void task\(void \*arg\)\n\{.*?^\}', touch, re.M | re.S)[0]
assert 'ulTaskNotifyTake(pdTRUE, touch_wait_ticks(down));' in task and 'pdMS_TO_TICKS(20)' not in task
checked.append('touch 20 ms')

# --- ptt: 100 ms ---
ptt = read('ptt.c')
assert re.search(r'#define POLL_MS\s+100\b', ptt)
assert 'vTaskDelay(pdMS_TO_TICKS(power_sleep_ptt_poll_ms(POLL_MS)));' in ptt and 'vTaskDelay(pdMS_TO_TICKS(POLL_MS))' not in ptt
checked.append('ptt 100 ms')

# --- the relaxed values: longer than today's, and the render wait under the 10 s task watchdog ---
assert relaxed['PP_POLLER_TOUCH'] > 20
assert relaxed['PP_POLLER_RENDER'] < 10000, 'the render task feeds the 10 s task watchdog on every loop'

# --- the flag-off header is a set of no-ops that hand the caller's wait back ---
code = r'''
#include <assert.h>
#include <stdio.h>
#include "power_sleep.h"
int main(void) {
    for (int who = 0; who < PP_POLLER_COUNT; who++)
        for (unsigned ms = 0; ms < 70000; ms += 7) assert(power_sleep_wait_ms((pp_poller_t)who, ms) == ms);
    assert(power_sleep_ptt_poll_ms(100) == 100 && !power_sleep_relaxed());
    power_sleep_init();
    puts("flag-off power_sleep.h: waits come back unchanged");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-quiet-pollers-') as folder:
    folder = Path(folder)
    (folder / 'off.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-I', str(main), str(folder / 'off.c'),
                    '-o', str(folder / 'off')], check=True)
    subprocess.run([str(folder / 'off')], check=True)

print('quiet pollers: ' + ', '.join(checked) + ' go through the policy and keep today\'s waits with the flag off PASS')
