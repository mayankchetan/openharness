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
relaxed['PP_POLLER_TOUCH'] = int(re.search(r'#else\n#define PP_TOUCH_POLL_MS (\d+)', (main / 'power_policy.h').read_text())[1])   # the D1 value (no touch wake)


def read(name):
    return (main / name).read_text()


checked = []

# --- habitat_touch: 20 ms idle, 4 ms during a contact; the contact wait is not a poller ---
touch = read('ui/habitat/touch_habitat.c')
helper = re.search(r'static TickType_t touch_wait_ticks\(bool down\)\n\{\n(.*?)\n\}', touch, re.S)
assert helper and 'power_sleep_wait_ms(PP_POLLER_TOUCH, 20)' in helper[1] and 'return pdMS_TO_TICKS(4);' in helper[1], 'touch wait helper'
assert 'if (ms > 200 && touch_sleep_parked())' in helper[1] and 'ms = 200;' in helper[1], 'a parked wake is polled every 200 ms'
task = re.search(r'static void task\(void \*arg\)\n\{.*?^\}', touch, re.M | re.S)[0]
assert 'ulTaskNotifyTake(pdTRUE, touch_wait_ticks(down));' in task and 'pdMS_TO_TICKS(20)' not in task
checked.append('touch 20 ms')

# --- ptt: 100 ms ---
ptt = read('ptt.c')
assert re.search(r'#define POLL_MS\s+100\b', ptt)
assert 'vTaskDelay(pdMS_TO_TICKS(power_sleep_ptt_poll_ms(POLL_MS)));' in ptt and 'vTaskDelay(pdMS_TO_TICKS(POLL_MS))' not in ptt
checked.append('ptt 100 ms')

# --- habitat_render: at most 1 s; the 5 s ceiling while the chip may sleep ---
render = read('ui/habitat/display_habitat.c')
assert 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_RENDER, wake_ms > 1000 ? 1000 : wake_ms)));' in render
checked.append('render 1000 ms')
# the guard timer that would cap every sleep at 1 s can be stopped and started, on the render task, only with the flag
guard = re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?void display_render_guard\(bool run\)\n\{(.*?)\n\}\n#endif', render, re.S)
assert guard, 'display_render_guard is compiled only with the flag'
assert 'esp_timer_stop(render_guard)' in guard[1] and 'esp_timer_start_periodic(render_guard, 1000000)' in guard[1]
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?void display_render_guard\(bool run\);\n(?:[^\n]*\n)*?#endif', read('ui/display.h'))

# --- cable_link reader: 100 ms ---
link = read('cable_link.c')
assert re.search(r'#define READ_WAIT_MS 100\b', link)
# The reader passes milliseconds to the transport; the USB transport turns them into ticks (cable_xport_usb.c).
assert 's_xport->read(s_xport, chunk, sizeof(chunk), power_sleep_wait_ms(PP_POLLER_CABLE_READER, READ_WAIT_MS))' in link
assert 'usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(wait_ms))' in read('cable_xport_usb.c')
checked.append('cable reader 100 ms')

# --- cable_session: 250 ms; no hello while the chip may sleep ---
client = read('cable_client.c')
session = re.search(r'static void session_task\(void \*arg\)\n\{.*?^\}', client, re.M | re.S)[0]
assert 'vTaskDelay(pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_CABLE_SESSION, 250)));' in session
assert 'if (!power_sleep_relaxed()) send_hello();' in session and session.count('send_hello()') == 1
checked.append('cable session 250 ms')

# --- proj_refresh: 1000 ms ---
app = read('app_main.c')
refresh = re.search(r'static void refresh_task\(void \*arg\)\n\{.*?^\}', app, re.M | re.S)[0]
assert 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_REFRESH, 1000)));' in refresh
checked.append('refresh 1000 ms')

# --- batlog: 1000 ms, stretched only to the next sample ---
batlog = read('battery_log.c')
assert 'uint32_t wait_ms = power_sleep_wait_ms(PP_POLLER_BATLOG, 1000);' in batlog
assert re.search(r'if \(wait_ms > 1000\) \{\s+const int64_t left_ms = \(next_us - esp_timer_get_time\(\)\) / 1000;\s+'
                 r'if \(left_ms < \(int64_t\)wait_ms\) wait_ms = left_ms < 1 \? 1 : \(uint32_t\)left_ms;\s+\}', batlog)
assert 'vTaskDelay(pdMS_TO_TICKS(wait_ms));' in batlog and 'vTaskDelay(pdMS_TO_TICKS(1000))' not in batlog
checked.append('batlog 1000 ms')

# --- Wi-Fi mode (S2 of the sleep + Wi-Fi work): the link's serve tick and backoff slices, and the mode switch task ---
net = read('net_link.c')
serve = re.search(r'static const char \*serve\(const cable_xport_t \*tls\)\n\{.*?^\}', net, re.M | re.S)[0]
assert re.search(r'#define SERVE_MS 50\b', net) and re.search(r'#define SLICE_MS 250\b', net)
assert 'nwifi_wait(NWIFI_DISCONNECTED, power_sleep_wait_ms(PP_POLLER_NET_SERVE, SERVE_MS))' in serve
assert 'SERVE_MS' not in serve.replace('PP_POLLER_NET_SERVE, SERVE_MS', '')
checked.append('net serve 50 ms')
sleep_ms = re.search(r'static void sleep_ms\(uint32_t ms\)\n\{.*?^\}', net, re.M | re.S)[0]
assert 'power_sleep_wait_ms(PP_POLLER_NET_BACKOFF, SLICE_MS)' in sleep_ms and 'ms < step ? ms : step' in sleep_ms
assert 'SLICE_MS' not in sleep_ms.replace('PP_POLLER_NET_BACKOFF, SLICE_MS', '')
# the join loop (join_wifi) is not a poller: it runs under PP_HOLD_LINK, when the dial must be awake
assert 'nwifi_wait(NWIFI_GOT_IP | NWIFI_DISCONNECTED, SLICE_MS)' in net
checked.append('net backoff 250 ms')
glue = read('mode_glue.c')
assert re.search(r'#define IDLE_MS 250\b', glue)
assert 'vTaskDelay(pdMS_TO_TICKS(power_sleep_wait_ms(PP_POLLER_MODE_SW, IDLE_MS)));' in glue and 'vTaskDelay(pdMS_TO_TICKS(IDLE_MS))' not in glue
checked.append('mode switch 250 ms')

# --- every poller in the policy table has exactly one caller ---
policy_h = (main / 'power_policy.h').read_text()
names = re.findall(r'^\s+(PP_POLLER_[A-Z_]+),?', re.search(r'typedef enum \{(.*?)\} pp_poller_t;', policy_h, re.S)[1], re.M)
assert len(names) == len(relaxed) + 1 and names[-1] == 'PP_POLLER_COUNT', names
callers = ''.join(p.read_text(errors='replace') for p in main.rglob('*.c') if p.name not in ('power_policy.c', 'power_sleep.c'))
for name in names[:-1]:
    assert len(re.findall(r'power_sleep_wait_ms\(' + name + r'\b', callers)) == 1, f'{name}: exactly one caller'
    assert name in relaxed, f'{name}: no relaxed value in power_policy.c'
    assert relaxed[name] > 20

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
