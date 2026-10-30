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
    if rel not in ('net_link.c', 'net_link.h', 'app_main.c', 'cable_client.c', 'cable_client.h', 'ui/habitat/ui_habitat.c', 'cable_link.c', 'cable_link.h'):
        assert 'net_link_' not in src, f'{rel} names net_link_*'

app = code('app_main.c')
assert len(re.findall(r'\bnet_link_start\(', app)) == 1
assert re.search(r'const bool wifi_mode = net_link_start\(\);\s*if \(!cable_client_start_with\(wifi_mode \? net_link_xport\(\) : cable_xport_usb\(\)\)\)', app)
assert app.index('reset_countdown();') < app.index('net_link_start('), 'the BOOT-held factory reset runs before the mode is chosen: it works in both modes'
assert not re.search(r'\bnet_link_start\(', code('cable_client.c')), 'only app_main starts Wi-Fi mode'
ui = code('ui/habitat/ui_habitat.c')
assert set(re.findall(r'\bnet_link_\w+', ui)) == {'net_link_status_text'}, 'the UI reads one caption and nothing else of the link'

net = code('net_link.c')
start_fn = body(net, 'net_link_start')
guard = start_fn.index('return false')
assert re.search(r'if \(!config_net_load\(&cfg\) \|\| cfg\.mode != 1\)', start_fn[:guard + 20]), 'the first thing net_link_start does is the mode check'
for first_use in ('heap_caps_calloc', 'xTaskCreate', 'ulTaskNotifyTake'):
    assert start_fn.find(first_use) > guard, f'{first_use} is missing or comes before the USB-mode return'
for never in ('nwifi_', 'esp_wifi_', 'cable_xport_tcp', 'cable_xport_tls', 'ui_'):
    assert never not in start_fn, f'net_link_start itself must not touch {never}: that is net_init_task'
assert len(re.findall(r'\bnwifi_start\(', net)) == 1
assert 'nwifi_start(' in body(net, 'net_init_task'), 'the bring-up runs in net_init'
assert 'xTaskCreate(net_init_task, "net_init", NET_INIT_STACK' in text('net_link.c'), 'net_init has an internal stack (xTaskCreate, not task_create_psram)'
assert 'task_create_psram(net_init_task' not in net

# ── 3. what the PSRAM-stack task can reach ──────────────────────────────────────────────────────────
FLASH = re.compile(r'\b(nvs_\w+|config_\w+|esp_ota_\w+|esp_partition_\w+|esp_flash_\w+|spi_flash_\w+|esp_phy_\w+|fw_update_\w+|esp_wifi_init|esp_wifi_start|nwifi_start|'
                   r'esp_mmap\w*|esp_vfs_\w+|fopen|fwrite|remove|rename|last_words_\w+)\b')
reachable = ['net_link_task', 'join_wifi', 'note_open_failure', 'log_stacks', 'serve', 'backoff', 'sleep_ms', 'ui_flush', 'post']
for name in reachable:
    assert not FLASH.search(body(net, name)), f'{name} (reachable from net_link) names a flash operation: {FLASH.search(body(net, name)).group(0)}'
task = body(net, 'net_link_task')
assert 'config_net_load' not in task and 'nwifi_start' not in task, 'net_link reads no NVS'
sta = code('wifi_sta.c')
for name in ('nwifi_connect', 'nwifi_disconnect', 'nwifi_bits', 'nwifi_wait', 'nwifi_log_ap', 'on_event'):
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
assert 'esp_wifi_set_ps(WIFI_PS_MIN_MODEM)' in start_sta
assert 'WIFI_PS_NONE' not in sta and 'WIFI_PS_MAX_MODEM' not in sta, 'min-modem always for now'
assert re.search(r'snprintf\(hostname, sizeof hostname, "dial-%02x", mac\[5\]\)', text('wifi_sta.c')), 'hostname dial-<last MAC byte, hex>'
assert 'esp_netif_set_hostname(netif, hostname)' in start_sta
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

# ── 6. the lines the bench reads ────────────────────────────────────────────────────────────────────
raw = text('net_link.c') + text('wifi_sta.c') + text('cable_xport_tcp.c')
for line in ('USB mode (', 'Wi-Fi mode (wl_mode = 1)', 'state %s -> %s', 'retry in %u ms (attempt %u, %s, state %s)', 'link up #%u handshake_ms=%d boot_ms=%d',
             'link down after %u ms (%s)', 'stacks %s: net_link free_min=%u B cable_link free_min=%d B', 'net_init done ok=%d stack free_min=%u B',
             'ram_telemetry_checkpoint("net_link_up")', 'ram_telemetry_checkpoint("wifi_started")', 'got ip " IPSTR " ms_to_ip=%d', 'disconnected reason=%d',
             'associated channel=%u authmode=%d', 'ap rssi=%d channel=%u', 'config ssid_len=%u pass_len=%u authmode_min=%s storage=ram hostname=%s',
             'handshake refused by the daemon (mbedTLS -0x%04X)', 'daemon unreachable (errno %d)', 'connected in %d ms'):
    assert line in raw, line

# ── 7. built into the image ─────────────────────────────────────────────────────────────────────────
cmake = text('CMakeLists.txt')
for source in ('cable_xport_tcp.c', 'net_state.c', 'log_ring.c', 'wifi_sta.c', 'net_link.c'):
    assert f'"{source}"' in cmake, source

print('Wi-Fi mode source rules: log hook queues only, radio code reachable only from net_link_start, '
      'net_init internal / net_link PSRAM with no flash call, W0 radio config, firmware refused off USB PASS')
