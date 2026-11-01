"""B4b: source rules for the phone mode's BLE link, checked without a toolchain (and without a dial: NimBLE is not on the host).

  1. THE DEVELOPMENT FLAG. BLE_DEV_ALLOW_UNBONDED (plan question QQ5: the walking skeleton runs with an unbonded phone, because TLS-PSK authenticates both
     ends) is a CMake variable on a command line. It is set in no defaults file and in no CMakeLists, it reaches the code only through one #ifdef in
     ble_link.c, it makes that file log a warning at every start, and nothing else in the firmware reads it. A product image therefore has it off.
  2. NO 2M PHY, EVER (finding FB1: a Pixel dropped the link when the dial asked for 2M with BLE 5 features off). No PHY call exists in any ble_* file.
  3. THE SHARED NUMBERS. Every constant here that also sits in test/vectors/ble_status.txt (PSM, MTU, MPS, DLE, advertising interval, write cap, the
     service UUID bytes) is equal to the vector file's value.
  4. THE ACCEPT POLICY is ble_accept_check, with the dev flag as its only waiver; a refusal for encryption reaches the peer as "insufficient encryption".
  5. THE LINK TASK'S CALL GRAPH holds no flash operation (it has a PSRAM stack); the key is read once, in ble_link_start, wiped, and never logged.
  6. THE HANDSHAKE: the chip is held awake across it, a channel close that arrives during it is posted after the verdict, and three alerts in a row are
     the only refusal.
  7. ISOLATION: NimBLE is included by ble_link.c alone; the transport (cable_xport_ble.c) and the state module know nothing of it; the default image has
     the inline stubs and none of this code.
"""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
fw = HERE / '..'
main = fw / 'main'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    """Source with comments and string literals blanked, so a rule about calls is not fooled by prose."""
    src = text(name)
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)


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


vectors = {}
for line in (fw / 'test/vectors/ble_status.txt').read_text(encoding='utf-8').replace('\r\n', '\n').split('\n'):
    f = line.split('\t')
    if f[0] == 'const':
        vectors[f[1]] = int(f[2])
    elif f[0] == 'uuid' and f[1] == 'service':
        vectors['service_le'] = f[3]
    elif f[0] == 'psm':
        vectors['psm'] = int(f[1])

link = code('ble_link.c')
raw_link = text('ble_link.c')
xport = code('cable_xport_ble.c')

# ── 1. the development flag ─────────────────────────────────────────────────────────────────────────
FLAG = 'BLE_DEV_ALLOW_UNBONDED'
for path in sorted(fw.iterdir()):
    if path.is_file() and path.name.startswith('sdkconfig.defaults'):
        assert FLAG not in path.read_text(), f'{path.name} sets the unbonded development flag'
top_cmake = (fw / 'CMakeLists.txt').read_text()
main_cmake = text('CMakeLists.txt')
assert FLAG not in top_cmake, 'the top-level CMakeLists must not mention the flag'
assert not re.search(r'set\s*\(\s*' + FLAG, main_cmake, re.I), 'main/CMakeLists.txt must not set the flag'
assert not re.search(r'option\s*\(\s*' + FLAG, main_cmake, re.I), 'the flag is not an option with a default'
guard = re.search(r'if\(CONFIG_BT_NIMBLE_ENABLED\)(.*?)\nendif\(\)', main_cmake, re.S)
assert guard, 'the phone mode is built only when NimBLE is'
assert 'target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BLE_LINK=1)' in guard[1]
assert '"ble_link.c"' in guard[1] and '"cable_xport_ble.c"' in guard[1], 'the BLE sources join the build only with NimBLE'
assert re.search(r'if\(' + FLAG + r'\)\s*target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE ' + FLAG + r'=1\)\s*endif\(\)', guard[1])
for name in ('"ble_link.c"', '"cable_xport_ble.c"'):
    assert main_cmake.count(name) == 1, f'{name} is in the build once, and only in the NimBLE block'
# the flag is read in ble_link.c only, in one #ifdef block that fixes DEV_UNBONDED, and a second #ifdef that logs the warning
def stripped(path):
    src = re.sub(r'/\*.*?\*/', ' ', path.read_text(errors='replace'), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


users = sorted(p.relative_to(main).as_posix() for p in main.rglob('*') if p.suffix in ('.c', '.h') and FLAG in stripped(p))
assert users == ['ble_link.c'], f'only ble_link.c uses the flag, in code: {users}'
assert re.search(r'#ifdef BLE_DEV_ALLOW_UNBONDED\n#define DEV_UNBONDED true\n#else\n#define DEV_UNBONDED false\n#endif', raw_link), 'off unless the build says so'
assert re.search(r'#ifdef BLE_DEV_ALLOW_UNBONDED\n\s*ESP_LOGW\(TAG, "DEVELOPMENT BUILD[^\n]*\n#endif', raw_link), 'a development build says so at every start'
assert len(re.findall(r'#\s*if\w*[^\n]*BLE_DEV_ALLOW_UNBONDED', raw_link)) == 2, 'two #ifdef blocks: the constant and the warning, and nothing else'
assert len(re.findall(r'\bDEV_UNBONDED\b', link)) == 5, 'DEV_UNBONDED: two definitions, the accept check, the encryption rule and the ENCRYPTED rule, and nothing else'
# nothing in the repository's build scripts under the firmware tree turns it on either
for path in list((fw / 'scripts').glob('*')) + list((fw / 'tools').rglob('*')):
    if path.is_file() and path.suffix in ('.sh', '.py', '.ps1', '.cmake', '.txt', '.md', ''):
        assert FLAG not in path.read_text(errors='replace') or path.suffix == '.md', f'{path.name} mentions the flag'

# ── 2. no PHY request ───────────────────────────────────────────────────────────────────────────────
for name in ('ble_link.c', 'cable_xport_ble.c', 'ble_state.c'):
    src = code(name)
    assert not re.search(r'ble_gap_(set_prefered|set_default_phy|read_le_phy|set_prefered_default)|LE_PHY|PHY_2M|PHY_CODED|_phy\b|ble_gap_ext', src), f'{name} touches the PHY'
assert 'ble_gap_set_data_len(' in link, 'the data length request stays'
assert not re.search(r'ble_gap_update_params|ble_gap_conn_rssi', link), 'no connection-parameter request in v1 (the Pixel chose 15 ms, latency 1)'

# ── 3. the shared numbers ───────────────────────────────────────────────────────────────────────────
def define(src, name):
    match = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|\d+)u?', src)
    assert match, name
    return int(match[1], 0)


assert define(link, 'COC_PSM') == vectors['psm'] == 0x0080
assert define(text('cable_xport_ble.h'), 'CABLE_XPORT_BLE_SDU_MAX') == vectors['coc_mtu_bytes'] == 4032
assert define(link, 'SDU_BLK') - 8 == vectors['coc_mps_bytes'] == 504, 'the MPS is the msys block size minus 8'
assert define(link, 'DLE_DELAY_MS') == vectors['dle_request_delay_ms']
assert define(link, 'DLE_OCTETS') == vectors['dle_octets'] == 251
assert 'BLE_GAP_ADV_ITVL_MS(50)' in link and vectors['adv_interval_ms'] == 50
header = text('cable_xport_ble.h')
default = re.search(r'#ifndef CABLE_XPORT_BLE_WRITE_CAP_MS\n#define CABLE_XPORT_BLE_WRITE_CAP_MS (\d+)', header)
assert default and int(default[1]) == vectors['write_cap_ms'] == 2000, 'the product write cap is the spec value; only the host test builds it shorter'
assert define(text("ble_state.h"), "BLE_ADV_TIMEOUT_MS") == vectors["adv_timeout_ms"]
assert define(text('ble_state.h'), 'BLE_ENC_DEADLINE_MS') == vectors['enc_deadline_ms']
assert define(text('ble_state.h'), 'BLE_TLS_DEADLINE_MS') == vectors['dial_tls_deadline_ms']
uuid = re.search(r'BLE_SERVICE_UUID_LE\[16\] = \{([^}]*)\}', text('ble_state.c'))
assert uuid and ''.join(f'{int(b, 16):02x}' for b in uuid[1].split(',')) == vectors['service_le'], 'the service UUID bytes of the vector file'
# the dial asks for the data length with the time the Pixel granted, nothing else
assert re.search(r'ble_gap_set_data_len\(conn, DLE_OCTETS, DLE_TIME_US\)', link)
# the L2CAP server is the fixed PSM with our MTU
assert 'ble_l2cap_create_server(COC_PSM, COC_MTU, coc_event, NULL)' in link

# ── 4. the accept policy ────────────────────────────────────────────────────────────────────────────
accept = body(link, 'coc_event')
assert 'ble_accept_check(enc, bonded, DEV_UNBONDED, s_chan != NULL)' in accept
assert re.search(r'case BLE_ACCEPT_REFUSE_ENC:.*?return BLE_HS_EENCRYPT;', accept, re.S), 'a refusal for encryption is "insufficient encryption" on the air'
assert re.search(r'case BLE_ACCEPT_OK:\s*return rx_arm\(event->accept\.chan\);', accept), 'only OK arms a receive buffer'
assert 'sec_state.encrypted' in link and 'sec_state.bonded' in link, 'the policy reads the link, not a flag of ours'
assert 'ble_encryption_required(DEV_UNBONDED)' in body(link, 'gap_event'), 'the 3 s deadline is waived only where the bond is'
assert re.search(r'if \(enc && \(bonded \|\| DEV_UNBONDED\)\) post\(BLE_EV_ENCRYPTED\);', body(link, 'gap_event')), 'ENCRYPTED needs an encrypted link and a bond (or the dev flag)'
assert 'ble_hs_cfg.sm_sc = 1' in link and 'ble_hs_cfg.sm_mitm = 1' in link and 'BLE_HS_IO_DISPLAY_YESNO' in link and 'ble_hs_cfg.sm_bonding = 1' in link

# ── 5. the link task: no flash operation, the key read once ─────────────────────────────────────────
FLASH = re.compile(r'\b(nvs_\w+|config_\w+|esp_ota_\w+|esp_partition_\w+|esp_flash_\w+|spi_flash_\w+|fw_update_\w+)\(')
for fn in ('ble_link_task', 'handshake', 'advertise', 'terminate_link', 'port_send', 'port_disconnect', 'port_rearm', 'port_peer_mtu', 'log_stacks',
           'x_read', 'x_write', 'x_is_up', 'post', 'current'):
    assert not FLASH.search(body(link, fn)), f'{fn} reaches a flash operation (the link task has a PSRAM stack)'
assert len(re.findall(r'config_ble_load\(', link)) == 1 and 'config_ble_load(&cfg)' in body(link, 'ble_link_start')
start = body(link, 'ble_link_start')
assert re.search(r'cable_xport_tls_wrap\(cable_xport_ble\(\), identity, cfg\.key, CFG_NET_KEY_LEN\);[^\n]*\n\s*zero\(&cfg, sizeof cfg\);', start), 'the key goes to the wrapper (which copies it), then is wiped'
for call in re.findall(r'ESP_LOG[EWIDV]\((.*?)\);', raw_link, re.S):
    assert not re.search(r'\bcfg\b|\.key\b|\bpsk\b', call), f'a log line names the key: {call[:80]}'
assert re.search(r'xTaskCreate\(ble_init_task, "ble_init", BLE_INIT_STACK, b, 5, NULL\)', raw_link), 'ble_init has an internal stack'
block = re.search(r'#if TASK_STACK_PSRAM_NET\s(.*?)#else\s(.*?)#endif', raw_link, re.S)
assert block and 'task_create_psram(' in block[1] and '"ble_link"' in block[1] and re.search(r'xTaskCreate\(', block[2]) and '"ble_link"' in block[2]
assert '#include "task_util.h"' in raw_link
assert define(link, 'BLE_LINK_STACK') == 8192 and define(link, 'BLE_INIT_STACK') >= 4096

# ── 6. the handshake ────────────────────────────────────────────────────────────────────────────────
hs = body(link, 'handshake')
assert hs.index('power_sleep_hold(PP_HOLD_LINK)') < hs.index('tls->open(tls)') < hs.index('power_sleep_release(PP_HOLD_LINK)'), 'the chip is held awake across the handshake'
assert 'net_hs_refused(err)' in hs and 'ble_handshake_failed(&s_ble, alert, now_ms())' in hs and 'cable_xport_tls_last_error(tls)' in hs
assert hs.index('s_handshaking = true') < hs.index('tls->open(tls)') and 's_handshaking = false' in hs.split('tls->open(tls)')[1]
assert 'if (closed) post(BLE_EV_CHANNEL_CLOSED);' in hs, 'a close during the handshake is posted after the verdict'
assert re.search(r'const bool defer = s_handshaking;\s*if \(defer\) s_close_pending = true;', accept)
task = body(link, 'ble_link_task')
assert 'ble_poll(&s_ble, now)' in task and 'BLE_EV_ADV_TIMEOUT' in task and 'BLE_EV_ENC_TIMEOUT' in task
assert 'cable_link_drain_logs(DRAIN_LINES)' in task and 'net_backoff_next_ms(&s_refused_ladder, true, esp_random())' in task, 'the refused ladder of net_state.h'
assert 'ble_gap_adv_stop()' in task and 'terminate_link()' in task
assert 'tls->close(tls)' in task, 'a finished session is closed before the next handshake'
tls_h = code('cable_xport_tls.c')
assert 'inner->kind == CABLE_XPORT_USB' in tls_h, 'the TLS wrapper refuses USB, and takes any other inner transport'

# ── 7. isolation ────────────────────────────────────────────────────────────────────────────────────
NIMBLE = re.compile(r'#\s*include\s*[<"](nimble/|host/ble_|host/util|os/os_mbuf|services/g|esp_bt)')
for path in sorted(main.rglob('*.c')) + sorted(main.rglob('*.h')):
    if NIMBLE.search(path.read_text(errors='replace')):
        assert path.name == 'ble_link.c', f'{path.name} includes NimBLE: only ble_link.c may'
for name in ('cable_xport_ble.c', 'cable_xport_ble.h'):
    assert not re.search(r'nimble|ble_gap|ble_l2cap|ble_hs|os_mbuf', code(name)), f'{name} knows NimBLE'
ble_h = text('ble_link.h')
assert '#ifdef DEVICE_BLE_LINK' in ble_h and 'static inline bool ble_link_start(void) { return false; }' in ble_h, 'the default image has the stubs'
assert not re.search(r'#\s*include\s*[<"](nimble|host/)', ble_h), 'the header is includable without NimBLE'
assert 'ble_link_start()' not in code('mode_glue.c') and 'ble_link_start(' not in code('cable_client.c')
# the BLE hello field and the reply field exist only where the image has the link
client = text('cable_client.c')
assert re.search(r'#ifdef DEVICE_BLE_LINK\n[^\n]*\n\s*if \(cable_link_transport\(\) == CABLE_XPORT_USB\) msg_string\(&root, "ble", "v1"\);\n#endif', client)

run = (fw / 'test/run.sh').read_text().replace('\r\n', '\n')
for name in ('test_cable_xport_ble.c', 'test_ble_link_source.py'):
    assert name in run, name

print('ble link: dev flag off everywhere but a command line, no PHY request, shared numbers equal the vectors, accept policy, no flash in the link task, '
      'key read once and never logged, handshake held awake, NimBLE confined to ble_link.c PASS')
