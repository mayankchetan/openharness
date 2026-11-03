"""W11: source rules for Wi-Fi mode, checked without a toolchain.

  1. The log hook never reaches a socket, lwIP or mbedTLS (finding F2): in Wi-Fi mode it queues the line in the ring, and nothing it can call
     in that mode names a network API.
  2. USB mode never starts the radio: esp_wifi_*, esp_netif_* and esp_event_* appear only in wifi_sta.c; nwifi_* only in wifi_sta.c and net_link.c;
     net_link_start() has one caller (app_main), returns before it creates anything unless wl_mode is 1 and the settings are whole, and the Wi-Fi
     bring-up is called from one place (the internal-stack net_init task).
  3. Stacks: net_init is internal; net_link is PSRAM under TASK_STACK_PSRAM_NET with an internal fallback, and nothing it can call reaches NVS,
     the PHY store, OTA, the partition table or the flash driver.
  4. The radio configuration of the W0 and bench6 runs: RAM storage, no NVS in the driver, min-modem power save, hostname dial-<last MAC byte>,
     IPv4 only, TCP_NODELAY, the password zeroed.
  5. Firmware slices and offers are refused unless the wire is USB.
  6. The lines the bench reads (gates G2, G3, G7) are logged.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    """Source with comments and string literals blanked, so a rule about calls is not fooled by prose."""
    src = text(name)
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)


def body(src, name):
    """The braces of function `name` (a definition at the start of a line), as text."""
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


NETWORK_API = re.compile(r'\b(send|recv|socket|connect|select|lwip_\w+|mbedtls_\w+|esp_tls\w*|cable_xport_t\w*|tcp_\w+|netconn_\w+|getaddrinfo)\s*\(')

# ── 1. the log hook ─────────────────────────────────────────────────────────────────────────────────
link = code('cable_link.c')
hook = body(link, 'log_vprintf')
assert 'log_ring_push(' in hook and 'async' in hook, 'the hook queues the line in Wi-Fi mode'
# In the async branch the only sink is the ring: send_locked appears only as the other arm of that one conditional expression.
assert re.search(r'async\s*\?\s*log_ring_push\([^;]*?\)\s*:\s*send_locked\(', hook), 'the ring is the async arm, send_locked the USB arm'
assert hook.count('send_locked(') == 1
assert not NETWORK_API.search(hook), 'the hook names a network API: ' + str(NETWORK_API.search(hook))
for banned in ('xQueueSend', 'xQueueReceive', 'vTaskDelay', 'xEventGroup', 'ulTaskNotify', 'portMAX_DELAY'):
    assert banned not in hook, banned
# what the hook calls in that mode: the ring, last_words, the formatter and a bounded try-lock
assert 'xSemaphoreTake(lock, pdMS_TO_TICKS(LOG_WRITE_WAIT_MS))' in hook, 'the lock is a bounded try, 5 ms'
ring = code('log_ring.c')
assert not NETWORK_API.search(ring), 'the ring names a network API'
for banned in ('xSemaphoreTake', 'xQueue', 'vTaskDelay', 'ulTaskNotify', 'xEventGroup', 'esp_log', 'ESP_LOG', 'portMAX_DELAY', 'malloc', 'heap_caps'):
    assert banned not in ring, f'log_ring.c must not wait, log or allocate: {banned}'
last_words = code('last_words.c')
assert not NETWORK_API.search(last_words) and 'lwip' not in last_words and 'mbedtls' not in last_words, 'last_words_add is reached from the hook'
for header in ('lwip', 'mbedtls', 'sys/socket', 'esp_tls', 'esp_netif', 'esp_wifi'):
    assert header not in text('cable_link.c'), f'cable_link.c includes {header}'
# the drain runs on the net task, not in the hook, and only sends through the ordinary send path
assert 'cable_link_drain_logs' not in hook
drain = body(link, 'cable_link_drain_logs')
assert 'cable_link_send(CABLE_TYPE_LOG' in drain and 'log_ring_pop(' in drain and 'is_up(' in drain
# dropped lines are counted, and the ring's own drops are part of the total
assert re.search(r'cable_link_dropped_logs\(void\)\s*\{[^}]*log_ring_overwritten\(&s_log_ring\)', link)
# USB mode creates no ring lock and starts no queue: it is created only for a non-USB kind
start = body(link, 'cable_link_start_with')
assert re.search(r'if \(xport->kind != CABLE_XPORT_USB\)\s*\{\s*s_fmt_lock = xSemaphoreCreateMutex\(\)', start)
assert start.index('log_ring_init(') > start.index('xport->kind != CABLE_XPORT_USB')
assert re.search(r'#define LOG_RING_BYTES 8192\b', link) and 'EXT_RAM_BSS_ATTR uint8_t s_log_ring_buf' in link, '8 KB, in PSRAM'

# ── 2. USB mode never starts the radio ──────────────────────────────────────────────────────────────
RADIO = re.compile(r'\b(esp_wifi_\w+|esp_netif_\w+|esp_event_\w+|esp_wifi\.h|esp_netif\.h|esp_event\.h)\b')
for path in sorted(main.rglob('*')):
    if path.suffix not in ('.c', '.h') or 'managed_components' in path.parts:
        continue
    rel = path.relative_to(main).as_posix()
    src = code(rel) if path.suffix == '.c' else re.sub(r'//[^\n]*', '', text(rel))
    if rel != 'wifi_sta.c' and rel != 'wifi_sta.h':
        found = RADIO.findall(src)
        assert not found, f'{rel} names the radio ({found[0]}); only wifi_sta.c may'
    if rel not in ('wifi_sta.c', 'wifi_sta.h', 'net_link.c'):
        assert 'nwifi_' not in src, f'{rel} names nwifi_*'
    if rel not in ('net_link.c', 'net_link.h', 'app_main.c', 'cable_client.c', 'cable_client.h', 'ui/habitat/ui_habitat.c', 'cable_link.c', 'cable_link.h',
                   'power_off.c'):   # power_off.c: net_link_close_for_power_off / net_link_resume only (S7; test_wifi_power_source.py pins the order)
        assert 'net_link_' not in src, f'{rel} names net_link_*'
    if rel == 'power_off.c':
        assert set(re.findall(r'\bnet_link_\w+', src)) == {'net_link_close_for_power_off', 'net_link_resume'}, 'power_off.c reaches nothing else of the link'

app = code('app_main.c')
assert len(re.findall(r'\bnet_link_start\(', app)) == 1
# W15: the boot decision (mode_glue_boot) gates it: a dial that decided USB (boot_usb with a host on the port, no wireless target) never starts the radio
assert re.search(r'const bool wifi_mode = mode_glue_boot\(\) == MS_MODE_WIFI && net_link_start\(\);\s*if \(!cable_client_start_with\(wifi_mode \? net_link_xport\(\) : cable_xport_usb\(\)\)\)', app)
assert app.index('reset_countdown();') < app.index('net_link_start('), 'the BOOT-held factory reset runs before the mode is chosen: it works in both modes'
assert not re.search(r'\bnet_link_start\(', code('cable_client.c')), 'only app_main starts Wi-Fi mode'
ui = code('ui/habitat/ui_habitat.c')
# W16: the Link screen adds net_link_state (the state alone, no text of the network); nothing else of the link is reachable from the UI
assert set(re.findall(r'\bnet_link_\w+', ui)) == {'net_link_status_text', 'net_link_state'}, 'the UI reads one caption, the state, and nothing else of the link'
assert 'atomic_load(&s_state)' in body(code('net_link.c'), 'net_link_state'), 'net_link_state is the state and nothing else'

net = code('net_link.c')
start_fn = body(net, 'net_link_start')
guard = start_fn.index('return false')
assert re.search(r'if \(!config_net_load\(&cfg\) \|\| cfg\.mode != 1\)', start_fn[:guard + 20]), 'the first thing net_link_start does is the mode check'
for first_use in ('heap_caps_calloc', 'xTaskCreate', 'xSemaphoreTake'):
    assert start_fn.find(first_use) > guard, f'{first_use} is missing or comes before the USB-mode return'
for never in ('nwifi_', 'esp_wifi_', 'cable_xport_tcp', 'cable_xport_tls', 'ui_'):
    assert never not in start_fn, f'net_link_start itself must not touch {never}: that is net_init_task'
assert len(re.findall(r'\bnwifi_start\(', net)) == 1
assert 'nwifi_start(' in body(net, 'net_init_task'), 'the bring-up runs in net_init'
assert 'xTaskCreate(net_init_task, "net_init", NET_INIT_STACK' in text('net_link.c'), 'net_init has an internal stack (xTaskCreate, not task_create_psram)'
assert 'task_create_psram(net_init_task' not in net

# ── 3. what the PSRAM-stack task can reach ──────────────────────────────────────────────────────────
FLASH = re.compile(r'\b(nvs_\w+|config_\w+|esp_ota_\w+|esp_partition_\w+|esp_flash_\w+|spi_flash_\w+|esp_phy_\w+|fw_update_\w+|esp_wifi_init|esp_wifi_start|esp_wifi_stop|esp_wifi_deinit|nwifi_start|nwifi_stop|'
                   r'esp_mmap\w*|esp_vfs_\w+|fopen|fwrite|remove|rename|last_words_\w+)\b')
reachable = ['net_link_task', 'join_wifi', 'note_open_failure', 'log_stacks', 'serve', 'backoff', 'sleep_ms', 'ui_flush', 'post', 'power_profile_sync']
for name in reachable:
    assert not FLASH.search(body(net, name)), f'{name} (reachable from net_link) names a flash operation: {FLASH.search(body(net, name)).group(0)}'
task = body(net, 'net_link_task')
assert 'config_net_load' not in task and 'nwifi_start' not in task, 'net_link reads no NVS'
sta = code('wifi_sta.c')
for name in ('nwifi_connect', 'nwifi_disconnect', 'nwifi_bits', 'nwifi_wait', 'nwifi_log_ap', 'on_event', 'net_power_profile'):
    assert not FLASH.search(body(sta, name)), f'{name} (reachable from net_link or the event task) names a flash operation'
for name in ('cable_xport_tcp.c', 'net_state.c', 'log_ring.c'):
    assert not FLASH.search(code(name)), f'{name} names a flash operation'
# the macro and the two creates
assert re.search(r'#ifndef TASK_STACK_PSRAM_NET\s+#define TASK_STACK_PSRAM_NET 1\b', text('task_util.h'))
assert 'NET_LINK_STACK 8192' in net and 'NET_INIT_STACK 5120' in net
assert 'for (;;)' in task, 'net_link never exits (a PSRAM-stack task must not delete itself)'
assert 'vTaskDelete' not in task and 'vTaskDelete(NULL)' in body(net, 'net_init_task'), 'only the internal-stack init task deletes itself'

# ── 4. the radio configuration ──────────────────────────────────────────────────────────────────────
start_sta = body(sta, 'nwifi_start')
assert 'init.nvs_enable = 0;' in start_sta and 'esp_wifi_set_storage(WIFI_STORAGE_RAM)' in start_sta
# every step of the bring-up is checked, and a failure takes down what was made (driver inited or not)
for call, inited in (('esp_wifi_init(&init)', 'false'), ('esp_wifi_set_storage(WIFI_STORAGE_RAM)', 'true'), ('esp_wifi_set_mode(WIFI_MODE_STA)', 'true'),
                     ('esp_wifi_set_config(WIFI_IF_STA, &wc)', 'true'), ('esp_wifi_start()', 'true')):
    assert re.search(r'e = ' + re.escape(call) + r';[^\n]*\n(?:[^\n]*\n)??\s*if \(e != ESP_OK\) return fail\(""' + r', e, ' + inited + r'\);', start_sta), call
fail_fn = body(sta, 'fail')
assert 'teardown(driver_inited)' in fail_fn and 'return false' in fail_fn
td = body(sta, 'teardown')
assert td.index('esp_wifi_stop()') < td.index('esp_wifi_deinit()') < td.index('esp_event_handler_unregister(WIFI_EVENT') < td.index('esp_netif_destroy_default_wifi(s_netif)')
assert body(sta, 'nwifi_stop').strip() == '{ teardown(true); }'
# log levels: the closed driver's tags are lowered to WARN before it starts, and this module's own tag is a different one
for tag in ('wifi', 'net80211', 'pp'):
    assert f'esp_log_level_set("{tag}", ESP_LOG_WARN)' in text('wifi_sta.c'), tag
assert text('wifi_sta.c').index('esp_log_level_set("wifi", ESP_LOG_WARN)') < text('wifi_sta.c').index('esp_wifi_start();')
assert 'static const char *TAG = "nwifi";' in text('wifi_sta.c'), 'our own lines must not share the driver tag that is lowered'
for phrase in ('LOG LEVELS', 'WARN and ERROR lines still pass', 'Invalid AP(<bssid>) SSID=<name>', 'PWD:%s'):
    assert phrase in text('wifi_sta.h'), phrase
assert 'net_power_profile(NET_POWER_ACTIVE)' in start_sta and 'esp_wifi_set_ps(' in body(sta, 'net_power_profile'), 'min-modem, set through hook 1 (W13; test_wifi_power_source.py)'
assert 'WIFI_PS_NONE' not in sta, 'power save is never switched off'
assert len(re.findall(r'WIFI_PS_MAX_MODEM', sta)) == 2, 'max-modem: the IDLE macro and the name table (2026-11-02), nowhere else'
assert re.search(r'snprintf\(hostname, sizeof hostname, "dial-%02x", mac\[5\]\)', text('wifi_sta.c')), 'hostname dial-<last MAC byte, hex>'
assert 'esp_netif_set_hostname(s_netif, hostname)' in start_sta
assert len(re.findall(r'listen_interval\s*=', sta)) == 1 and 'wc.sta.listen_interval = 3;' in start_sta, 'listen interval set once, in the association config'
assert 'zero(&wc, sizeof wc)' in start_sta, 'the driver config copy of the password is scrubbed'
assert 'wc.sta.pmf_cfg.capable = true' in start_sta
assert start_sta.index('esp_wifi_set_config(') < start_sta.index('zero(&wc') < start_sta.index('esp_wifi_start()'), 'scrubbed right after it is handed over'
tcp = code('cable_xport_tcp.c')
assert 'AF_INET,' in tcp and 'AF_INET6' not in tcp and 'AF_UNSPEC' not in tcp and 'getaddrinfo' not in tcp and 'gethostbyname' not in tcp, 'IPv4 only, no DNS'
assert 'TCP_NODELAY' in tcp and 'sk_set_nodelay(fd)' in body(tcp, 'tcpx_open')
assert 'CONNECT_MS 5000' in text('cable_xport_tcp.c')
assert re.search(r'#define READ_WAIT_MS 100\b', text('cable_link.c')), 'the reader waits at most 100 ms per read'
tcpx_write = body(tcp, 'tcpx_write')
assert 'memcpy' not in tcpx_write and 'malloc' not in tcpx_write and 'static' not in tcpx_write, 'write keeps no copy of the caller buffer'
assert 'again(errno)) return 0' in tcpx_write, 'EAGAIN is 0, not an error'
# the wrapper, not this transport, carries TLS
assert 'mbedtls' not in tcp.lower() and 'esp_tls' not in tcp
assert re.search(r'cable_xport_tls_wrap\(cable_xport_tcp\(\), identity, b->cfg\.key, CFG_NET_KEY_LEN\)', net)
assert 'device_mac_str(identity' in net
assert re.search(r'zero\(b->cfg\.key, sizeof b->cfg\.key\)', net) and re.search(r'zero\(b->cfg\.pass, sizeof b->cfg\.pass\)', net)
assert 'zero(&b->cfg, sizeof b->cfg)' in body(net, 'net_init_task')
# no secret in any log call of the Wi-Fi files
for name in ('net_link.c', 'wifi_sta.c', 'cable_xport_tcp.c'):
    for statement in re.findall(r'ESP_LOG[EWIDV]\s*\((?:[^;]|\n)*?\)\s*;', re.sub(r'//[^\n]*', '', text(name))):
        args = re.sub(r'"(?:[^"\\]|\\.)*"', '""', statement)
        for word in re.findall(r'\b(?:pass|key|psk|secret|ssid|cfg)\w*', args):
            assert word.endswith('_len'), f'{name}: a log call takes secret material ({word}): {statement}'

# ── 5. firmware is USB only ─────────────────────────────────────────────────────────────────────────
client = text('cable_client.c')   # strings kept: the checks below name message types
frame_fw = re.search(r'if \(type == CABLE_TYPE_FW\) \{(.*?)\n    \}', client, re.S).group(1)
assert re.match(r'\s*(//[^\n]*\n\s*)*if \(cable_link_transport\(\) != CABLE_XPORT_USB\) \{', frame_fw), 'the 0x03 frame check comes first'
assert re.search(r'^\s*if \(cable_link_transport\(\) != CABLE_XPORT_USB\) \{\s*s_unknown\+\+;\s*return;\s*\}', frame_fw, re.M)
offer = re.search(r'strcmp\(t, "fw\.offer"\) == 0\) \{(.*?)\n    \}', client, re.S).group(1)
assert re.match(r'\s*if \(cable_link_transport\(\) != CABLE_XPORT_USB\) \{[^\n]*\n\s*cable_client_fw_error\("not supported"\);\s*return;', offer), 'the offer check comes first'
assert 'fw_update_offer' in offer and offer.index('cable_link_transport()') < offer.index('fw_update_offer')
# a radio link that closed ends the session at once (the 15 s silence rule stays, for a daemon that is merely quiet)
tick = body(client, 'session_tick')
assert re.search(r'cable_link_transport\(\) != CABLE_XPORT_USB && !cable_link_host_present\(\)\) \{\s*session_down\("link"\);', tick)
# net.* is USB only (also pinned in test_cable_net_set.py)
assert 'cable_link_transport() != CABLE_XPORT_USB) { net_reply(id, false, "transport", false); goto done; }' in client

# ── 5b. start-up hand-off: ownership of the boot block, no notification of a task that may be gone ──────────────────
raw_net = text('net_link.c')
assert 'xTaskNotifyGive' not in raw_net and 'ulTaskNotifyTake' not in raw_net and 'TaskHandle_t waiter' not in raw_net, 'no task notification in the hand-off'
boot_struct = re.search(r'typedef struct \{\s*config_net_t cfg;(.*?)\} boot_t;', net, re.S).group(1)
assert 'SemaphoreHandle_t done' in boot_struct and 'atomic_int phase' in boot_struct and 'atomic_int refs' in boot_struct
rel = body(net, 'boot_release')
assert 'atomic_fetch_sub(&b->refs, 1) != 1) return' in rel and 'vSemaphoreDelete(b->done)' in rel and 'free(b)' in rel
assert len(re.findall(r'\bfree\(b\)', net)) == 1, 'b is freed in one place: boot_release, by whoever touches it last'
init_fn = body(net, 'net_init_task')
assert 'atomic_compare_exchange_strong(&b->phase, &expected, PHASE_COMMITTED)' in init_fn, 'net_init commits before it starts net_link'
assert init_fn.index('PHASE_COMMITTED') < init_fn.index('atomic_store(&s_tls, tls)') < init_fn.index('task_create_psram(net_link_task') , 'the point of no return comes first'
assert init_fn.index('xSemaphoreGive(b->done)') < init_fn.index('boot_release(b)') < init_fn.index('vTaskDelete(NULL)')
assert init_fn.count('boot_release(b)') == 1 and 'b->' not in init_fn[init_fn.index('boot_release(b)'):], 'net_init does not touch b after releasing it'
start_fn2 = body(net, 'net_link_start')
assert 'atomic_compare_exchange_strong(&b->phase, &expected, PHASE_ABANDONED)' in start_fn2
abandon = start_fn2[start_fn2.index('PHASE_ABANDONED'):]
assert abandon.index('boot_release(b)') < abandon.index('return false'), 'on timeout the caller lets go of b and returns'
assert 'b->' not in abandon[abandon.index('boot_release(b)'):abandon.index('return false')], 'and does not touch b after that'
assert 'xSemaphoreTake(b->done, portMAX_DELAY)' in start_fn2, 'a committed start-up is waited for to the end (a task create away)'
assert start_fn2.count('atomic_store(&b->refs, 1)') == 2, 'with no net_init task there is one holder'

# ── 5c. failure fallbacks: whatever net_init made is taken down, and the caller falls back to USB ───────────────────
undo_fn = body(net, 'undo')
assert undo_fn.index('post(NET_EV_STOP)') < undo_fn.index('atomic_store(&s_tls, NULL)') < undo_fn.index('nwifi_stop()') < undo_fn.index('cable_xport_tls_free(tls)')
assert 'if (radio) nwifi_stop()' in undo_fn and 'if (tls) cable_xport_tls_free(tls)' in undo_fn
assert re.search(r'if \(!b->ok\) undo\(tls, radio\);', init_fn), 'every failed path of net_init ends in undo()'
assert init_fn.index('if (!b->ok) undo(') > init_fn.index('} while (0);')
# the failure points all break out of the do-while; none returns early past undo()
do_block = init_fn[init_fn.index('do {'):init_fn.index('} while (0);')]
assert 'return' not in do_block and do_block.count('break;') >= 6, 'no early return inside the start-up block'
# a failed net_link task create is one of them
assert re.search(r'if \(task_ok != pdPASS\) \{[^}]*break;', do_block)
# a failed nwifi_start leaves nothing running (it takes its own pieces down) and the TLS wrapper is freed by undo()
assert re.search(r'if \(!nwifi_start\(b->cfg\.ssid, b->cfg\.pass, post\)\) break;', do_block)
assert 'radio = true;' in do_block and do_block.index('nwifi_start') < do_block.index('radio = true;')
# the caller of a failed start-up gets false and app_main runs USB mode (test above: wifi_mode ? net_link_xport() : cable_xport_usb())
assert 'return ok;' in start_fn2 and 'const bool ok = finished && b->ok;' in start_fn2

# ── 5d. refusals: only an alert counts, and three in a row ───────────────────────────────────────────────────
hs = re.sub(r'//[^\n]*', '', text('net_hs_class.h'))
assert re.search(r'return mbedtls_error == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE;', hs) and hs.count('MBEDTLS_ERR_') == 1, 'one code: the alert'
note = body(net, 'note_open_failure')
assert 'net_handshake_event(&s_refusals, alert)' in note and 'post(ev)' in note and 'post(NET_EV_TLS_REFUSED)' not in note
assert 'net_refusal_reset(&s_refusals)' in note, 'a TCP failure breaks the run'
assert 'net_refusal_reset(&s_refusals);\n        post(NET_EV_LINK_UP)' in net.replace('\r', ''), 'a link up clears the count'
st = code('net_state.c')
assert 'NET_REFUSALS_TO_AUTH_FAILED' in code('net_state.h') and re.search(r'#define NET_REFUSALS_TO_AUTH_FAILED 3\b', code('net_state.h'))
assert 'return keep_wifi ? "Open Link screen" : "Plug in USB";' in text('net_state.c') and 'Key refused' not in text('net_state.c'), 'the caption for a refused key is the way out (W16d): plug in USB; a factory reset erases everything and is not it'
assert 'W14' not in text('net_state.c'), 'no stale note that the USB recovery is still to come'

# ── 5e. reader stack and ring lock discipline ─────────────────────────────────────────────────────────────
link_raw = text('cable_link.c')
assert re.search(r'#define READER_STACK 6144\b', link_raw) and re.search(r'#define READER_STACK_RADIO 8192\b', link_raw)
assert re.search(r'#if TASK_STACK_PSRAM_READER\s+const uint32_t reader_stack = xport->kind == CABLE_XPORT_USB \? READER_STACK : READER_STACK_RADIO;', link_raw)
assert 'task_create_psram(reader_task, "cable_link", reader_stack' in link_raw, 'the larger stack only exists where the stack is in PSRAM'
ring_src = code('log_ring.c')
for locked in re.findall(r'RING_LOCK\(\);(.*?)RING_UNLOCK\(\);', ring_src, re.S):
    assert not re.search(r'\b(memcpy|memmove|memset|put|get)\s*\(', locked), 'a copy of a line happens inside the ring lock: ' + locked[:80]
assert ring_src.count('RING_LOCK();') == ring_src.count('RING_UNLOCK();') >= 5
assert "log_ring_write(r, &slot, line);\n    log_ring_commit(r, &slot);" in ring_src.replace('\r', ''), 'push is reserve, copy outside the lock, commit'

# ── 6. the lines the bench reads ────────────────────────────────────────────────────────────────────
raw = text('net_link.c') + text('wifi_sta.c') + text('cable_xport_tcp.c')
for line in ('USB mode (', 'Wi-Fi mode (wl_mode = 1)', 'state %s -> %s', 'retry in %u ms (attempt %u, %s, state %s)', 'link up #%u handshake_ms=%d boot_ms=%d',
             'link down after %u ms (%s)', 'stacks %s: net_link free_min=%u B cable_link free_min=%d B', 'net_init done ok=%d stack free_min=%u B',
             'ram_telemetry_checkpoint("net_link_up")', 'ram_telemetry_checkpoint("wifi_started")', 'got ip " IPSTR " ms_to_ip=%d', 'disconnected reason=%d',
             'associated channel=%u authmode=%d', 'ap rssi=%d channel=%u', 'config ssid_len=%u pass_len=%u authmode_min=%s storage=ram hostname=%s',
             'handshake answered with an alert (mbedTLS -0x%04X), %u in a row', 'handshake failed without an alert (mbedTLS -0x%04X)',
             'daemon unreachable (errno %d)', 'connected in %d ms'):
    assert line in raw, line

# ── 7. built into the image ─────────────────────────────────────────────────────────────────────────
cmake = text('CMakeLists.txt')
for source in ('cable_xport_tcp.c', 'net_state.c', 'log_ring.c', 'wifi_sta.c', 'net_link.c'):
    assert f'"{source}"' in cmake, source

print('Wi-Fi mode source rules: log hook queues only, radio code reachable only from net_link_start, '
      'net_init internal / net_link PSRAM with no flash call, W0 radio config, firmware refused off USB PASS')
