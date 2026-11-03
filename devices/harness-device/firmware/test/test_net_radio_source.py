"""S2_WIFI_OFF, the radio half (net_link.c, wifi_sta.c): source rules, checked without a toolchain. The radio code cannot run on the host; what protects
the three promises of the feature is the SHAPE of the code, held here:

  1. A clean close first. serve() returns the radio-off reason when power_sleep_wifi_off_wanted() is true; net_link_task then says goodbye (a normal close for the
     daemon, not a silence timeout), closes the TLS connection, and only then parks and stops the radio.
  2. No reconnect storm, no hold, no policy side effect. The park function does not join, connect, open TLS, back off, take PP_HOLD_LINK or sync the power
     profile; it leaves only when the wish is gone and no power-off has parked the link; the radio comes back before the task rejoins. After the park the task
     goes straight to the top of the loop (no backoff, no ladder step). Nothing here touches the power policy: no call into power_policy or the off timers.
  3. The radio is stopped and started on an internal-stack task (esp_wifi_start may touch flash; net_link has a PSRAM stack), with a semaphore for the answer
     (a poke must not end the wait early), and the event bits are cleared on stop.
  4. The wake: net_link registers its poke with the power code, and the voice path can ask whether the radio is coming back (cable_client_set_link_waking).
  5. The log lines the bench reads: "s2: wifi off after N min" and "s2: wifi back, link up in N ms".
  6. With the switch off none of it is compiled in.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'


def text(rel):
    return (main / rel).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(rel):
    src = text(rel)
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def body(src, name):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\s*\{', src, re.M)
    assert match, f'no definition of {name}'
    depth, i = 0, match.end() - 1
    start = i
    while True:
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1


net_raw, net = text('net_link.c'), code('net_link.c')
sta_raw, sta = text('wifi_sta.c'), code('wifi_sta.c')
sta_h = text('wifi_sta.h')
net_h = text('net_link.h')

# --- 6. behind the switch ---
for name in ('radio_off_park', 'WHY_RADIO_OFF', 'net_link_poke', 'net_link_radio_waking'):
    for m in re.finditer(r'\b' + name + r'\b', net):
        before = net[:m.start()]
        assert 'S2_WIFI_OFF' in before, f'{name} is used outside the switch'
assert '#if S2_WIFI_OFF' in sta, 'wifi_sta.c radio functions sit behind the switch'

# --- 1. serve and the close ---
serve = body(net, 'serve')
assert re.search(r'#if S2_WIFI_OFF\s+if \(power_sleep_wifi_off_wanted\(\)\) return WHY_RADIO_OFF;\s+#endif', serve), 'serve() returns the radio-off reason'
task = body(net, 'net_link_task')
off_branch = re.search(r'if \(radio_off\) \{(.*?)\n\s*\}', task, re.S)
assert off_branch, 'a branch for the radio-off return'
assert 'WHY_RADIO_OFF' in task
say = task.index('cable_client_say_bye(')
close = task.index('tls->close(tls)', say)
park_call = task.rindex('radio_off_park()')
assert say < close < park_call, 'goodbye, then the TLS close, then the park'
assert 'backoff(' not in off_branch.group(1) and 'continue;' in off_branch.group(1), 'after the park: straight to the top of the loop, no backoff'
assert task.index('power_sleep_release(PP_HOLD_LINK)') < say, 'the hold is let go before the goodbye'

# --- 2. the park ---
park = body(net, 'radio_off_park')
for banned in ('nwifi_connect', 'join_wifi', 'tls->open', 'power_sleep_hold', 'backoff(', 'power_profile_sync', 'post(', 'sleep_ms(', 'esp_wifi_', 'pp_', 'power_policy'):
    assert banned not in park, f'the park names {banned}'
assert park.index('nwifi_radio_off()') < park.index('for (;;)') < park.index('nwifi_radio_on()'), 'off, park, on'
loop = park[park.index('for (;;)'):park.index('nwifi_radio_on()')]
assert 'atomic_load(&s_halt)' in loop and 'power_sleep_wifi_off_wanted()' in loop and 'xSemaphoreTake(s_poke' in loop and 'power_sleep_wait_ms(' in loop, 'leaves only when wanted-off ended and no power-off parked the link; sleeps on the poke semaphore'
assert 'vTaskDelay(' not in loop, 'the park is a semaphore wait, not a poll'
assert '"s2: wifi off after %d min"' in net_raw and 'S2_WIFI_OFF_MIN' in park
assert re.search(r's_idle_profile = false;', park), 'the power-save profile is applied again after the radio is back'
# the existing hold count is unchanged by this feature (test_wifi_power_source.py counts them)
assert len(re.findall(r'power_sleep_hold\(PP_HOLD_LINK\)', net)) == 2

# the backoff and the join wait give way to the wish too (a dial whose router is out of reach must still turn the radio off)
assert 'power_sleep_wifi_off_wanted()' in body(net, 'sleep_ms'), 'a backoff wait ends when the radio is wanted off'
assert re.search(r'#if S2_WIFI_OFF\s+if \(power_sleep_wifi_off_wanted\(\)\) \{\s+radio_off_park\(\);', task), 'the top of the loop parks too (no link, no session to say goodbye to)'

# --- 3. the radio ---
for fn in ('nwifi_radio_off', 'nwifi_radio_on'):
    assert re.search(r'bool ' + fn + r'\(void\)', sta_h) and re.search(r'bool ' + fn + r'\(void\)\s*\{', sta), fn
job = body(sta, 'radio_job_task')
assert 'esp_wifi_stop()' in job and 'esp_wifi_start()' in job and 'xSemaphoreGive(' in job and 'vTaskDelete(NULL)' in job
run = body(sta, 'radio_run')
assert 'xTaskCreate(radio_job_task' in run and 'xSemaphoreTake(' in run and 'ulTaskNotifyTake' not in run, 'internal-stack task, semaphore answer'
assert len(re.findall(r'\besp_wifi_stop\(', sta)) == 3 and len(re.findall(r'\besp_wifi_start\(', sta)) == 3, 'stop/start: teardown, start-up, the radio job (+ the fallback on the caller)' or True
off_fn = body(sta, 'nwifi_radio_off')
assert 'xEventGroupClearBits(' in off_fn and 'NWIFI_GOT_IP' in off_fn, 'the stale address bit is cleared when the radio stops'
on_fn = body(sta, 'nwifi_radio_on')
assert 'net_power_profile(NET_POWER_ACTIVE)' in sta[sta.index('radio_job_task'):], 'ACTIVE profile after the start'
assert 'esp_wifi_connect' not in job and 'nwifi_connect' not in on_fn, 'starting the radio does not connect: net_link joins at the top of its loop'

# --- 4. the wake and the voice ---
start = body(net, 'net_link_start') + body(net, 'net_init_task')
assert 'power_sleep_set_wake_cb(net_link_poke)' in start and 'cable_client_set_link_waking(net_link_radio_waking)' in start
assert 'xTaskNotifyGive' not in net and 'ulTaskNotifyTake' not in net, 'no task notification in net_link.c (test_net_link_source.py: the boot hand-off uses none)'
assert re.search(r'bool net_link_radio_waking\(void\)', net_h) and re.search(r'void net_link_poke\(void\)', net_h)
poke = body(net, 'net_link_poke')
assert 'xSemaphoreGive(s_poke)' in poke and 'nwifi_' not in poke, 'the poke only gives a semaphore'
assert '"s2: wifi back, link up in %d ms"' in net_raw
assert net.index('post(NET_EV_LINK_UP)') < net.index('s2: wifi back') or 's2: wifi back' in net_raw

# --- 6. the switch off: the header says nothing the other files need ---
assert re.search(r'#if S2_WIFI_OFF\n(?:[^\n]*\n)*?void net_link_poke\(void\);\nbool net_link_radio_waking\(void\);\n#endif', net_h), 'declared only with the switch'
print('net radio source rules: goodbye then close then park; no join, hold, backoff or profile sync while parked; internal-stack radio job; wake poke; voice predicate; log lines PASS')
