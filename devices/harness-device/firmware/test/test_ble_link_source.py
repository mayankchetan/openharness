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
# B4e (review M2): the top level reads the flag in ONE block, before project(): a configure warning and the version suffix +bledev (the image's own
# esp_app_desc version says it is a development image). Nowhere else, and it never sets the flag.
top_code = re.sub(r'#[^\n]*', '', top_cmake)
dev_block = re.search(r'if\(' + FLAG + r'\)(.*?)\nendif\(\)', top_code, re.S)
assert dev_block, 'the top-level CMakeLists has the one block for the development flag'
assert 'message(WARNING' in dev_block[1] and 'set(PROJECT_VER "${_dev_base_ver}+bledev")' in dev_block[1] and 'version.txt' in dev_block[1]
assert FLAG not in top_code.replace(dev_block[0], ''), 'the flag is read in that block and nowhere else at the top level'
assert not re.search(r'(?<![A-Za-z_])set\s*\(\s*' + FLAG, top_code), 'the top level does not set the flag'
assert top_code.index(dev_block[0]) < top_code.index('project(interns_commander)'), 'PROJECT_VER is set before project()'
assert not re.search(r'(?<![A-Za-z_])set\s*\(\s*' + FLAG, main_cmake, re.I), 'main/CMakeLists.txt must not set the flag'
# and it does not persist: read once in the component, then removed from the cache (a later build in the same directory without -D is a product build)
assert 'unset(' + FLAG + ' CACHE)' in main_cmake
assert main_cmake.index('unset(' + FLAG + ' CACHE)') > main_cmake.index('target_compile_definitions(${COMPONENT_LIB} PRIVATE ' + FLAG + '=1)')
assert main_cmake.count('unset(' + FLAG) == 1
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
assert len(re.findall(r'\bDEV_UNBONDED\b', link)) == 6, 'DEV_UNBONDED: two definitions, the accept check, the encryption rule, the ENCRYPTED rule and the characteristic write gate, and nothing else'
# nothing in the repository's build scripts under the firmware tree turns it on either
for path in list((fw / 'scripts').glob('*')) + list((fw / 'tools').rglob('*')):
    if path.is_file() and path.suffix in ('.sh', '.py', '.ps1', '.cmake', '.txt', '.md', ''):
        assert not re.search(FLAG + r'\s*[=:]\s*(1|ON|TRUE|YES)', path.read_text(errors='replace'), re.I), f'{path.name} turns the development flag on (the guards may name it, never set it)'

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
assert 'ble_accept_check(&sec, DEV_UNBONDED, s_chan != NULL)' in accept and 'link_sec(event->accept.conn_handle, &sec)' in accept
assert re.search(r'case BLE_ACCEPT_REFUSE_ENC:.*?return BLE_HS_EENCRYPT;', accept, re.S), 'a refusal for encryption is "insufficient encryption" on the air'
assert re.search(r'case BLE_ACCEPT_OK:\s*return rx_arm\(event->accept\.chan\);', accept), 'only OK arms a receive buffer'
lsec = body(link, 'link_sec')
for field in ('encrypted', 'bonded', 'authenticated', 'key_size'):
    assert f'sec_state.{field}' in lsec, f'the policy reads {field} from the link, not from a flag of ours'
assert 'ble_encryption_required(DEV_UNBONDED)' in body(link, 'gap_event'), 'the 3 s deadline is waived only where the bond is'
note = body(link, 'note_security')
assert 'ble_sec_trusted(&sec)' in note and 'ble_set_trusted(&s_ble, true)' in note, 'a link that passes the rule is marked trusted before ENCRYPTED'
assert re.search(r'if \(trusted \|\| \(DEV_UNBONDED && sec\.encrypted\)\) post\(BLE_EV_ENCRYPTED\);', note), 'ENCRYPTED needs a trusted link (or, in the dev build, any encrypted one)'
assert 'note_security(event->enc_change.conn_handle)' in body(link, 'gap_event')
assert 'ble_hs_cfg.sm_sc = 1' in link and 'ble_hs_cfg.sm_mitm = 1' in link and 'BLE_HS_IO_DISPLAY_YESNO' in link and 'ble_hs_cfg.sm_bonding = 0;' in link

# ── 4b. B4c: the dial asks for encryption itself, and the three characteristics are guarded as the spec says ─────────────────────────────
# The dial does not wait for the phone to start encryption: it calls ble_gap_security_initiate after the connect, in the product build only (a
# development build waives the bond, so it has no use for a pairing that would need the screen). One call site.
gap = body(link, 'gap_event')
assert code('ble_link.c').count('ble_gap_security_initiate(') == 1, 'one security request in the file'
assert re.search(r'if \(ble_encryption_required\(DEV_UNBONDED\)\) \{[^}]*ble_gap_security_initiate\(s_conn\)', gap, re.S), 'asked for in the connect event, only where a bond is wanted'
assert gap.index('case BLE_GAP_EVENT_CONNECT:') < gap.index('ble_gap_security_initiate(') < gap.index('case BLE_GAP_EVENT_DISCONNECT:')
# Fix B (2026-11-09): and only while the pairing window is open or for a peer that is already bonded. A request to a stranger makes Android raise a consent
# prompt that nobody can answer in 3 s. The question is the pure ble_security_request_wanted, asked under the lock with the window and the peer's bond.
connect = re.search(r'case BLE_GAP_EVENT_CONNECT:(.*?)case BLE_GAP_EVENT_DISCONNECT:', gap, re.S)[1]
assert 'const bool bonded_peer = peer_bonded(s_conn, bonds, &by_identity);' in connect
assert re.search(r'state_lock\(\);\s*const bool window_open = ble_pair_is_open\(&s_pair, now_ms\(\)\);\s*const bool ask = ble_security_request_wanted\(&s_pair, now_ms\(\), bonded_peer\);\s*state_unlock\(\);', connect), 'the window is read under the lock'
assert 'ask ? ble_gap_security_initiate(s_conn) : 0' in connect, 'the one security request is behind the gate'
assert connect.index('peer_bonded(s_conn') < connect.index('ble_security_request_wanted(') < connect.index('ble_gap_security_initiate(s_conn)')
pb = body(link, 'peer_bonded')
assert 'ble_gap_conn_find(conn, &d)' in pb and 'key.peer_addr = d.peer_id_addr;' in pb and 'ble_store_read_our_sec(&key, &val) == 0' in pb, 'bonded = the host store holds our keys for this identity address'
# Item 4d: the lookup is by the IDENTITY address the stack resolved; an unresolved private address with a bond stored may still be the bonded phone (pure rule, host-tested)
assert 'ble_peer_may_hold_bond(*by_identity, bonds, d.peer_ota_addr.type, d.peer_ota_addr.val)' in pb, 'the fallback is the pure rule'
sdk = (fw / 'sdkconfig.defaults.ble').read_text()
assert 'BT_NIMBLE_HS_PVCY=n' not in sdk and 'BT_NIMBLE_SM_SC=y' in sdk, 'privacy (RPA resolving) stays at the IDF default: on'
assert 'BLE_SM_PAIR_KEY_DIST_ID' in body(link, 'apply_bonding_policy') and body(link, 'apply_bonding_policy').count('BLE_SM_PAIR_KEY_DIST_ID') == 2, 'both sides distribute the ID key when the window is open'
# Item 4a: the auto-open is asked again at every connect, before the security request is decided, and an open window is not extended
assert re.search(r'const unsigned bonds = ble_link_bond_count\(\);\s*state_lock\(\);\s*const bool rearm = ble_pair_rearm_wanted\(&s_pair, now_ms\(\), s_started, bonds\);\s*state_unlock\(\);\s*if \(rearm\) window_set\(true, ', connect), 'the re-arm: bond count outside the lock, rule under it'
assert connect.index('ble_pair_rearm_wanted(') < connect.index('peer_bonded(s_conn') < connect.index('ble_security_request_wanted('), 're-arm, then the peer, then the request decision'
assert code('ble_link.c').count('ble_pair_rearm_wanted(') == 1 and code('ble_link.c').count('window_set(true') == 2, 'two openers: the start and the connect (the Phone screen is the third, through ble_link_pairing_window)'
# Item 4c: every open and close of the window has a reason in the log; the expiry is logged by the link task; the bond count is logged at the start; the request says why
wset = body(link, 'window_set')
assert 'ble_pair_open(&s_pair, now_ms())' in wset and 'ble_pair_close(&s_pair)' in wset and 'ESP_LOGI(TAG, "pairing window %s: %s"' in body(raw_link, 'window_set'), 'one place opens and closes, with a reason'
assert re.findall(r'\bble_pair_(?:open|close)\(&s_pair', code('ble_link.c')) == ['ble_pair_open(&s_pair', 'ble_pair_close(&s_pair'], 'nothing opens or closes the window except window_set'
assert 'window_set(open, "asked by the Phone screen")' in body(raw_link, 'ble_link_pairing_window')
assert 'window_set(false, "forget")' in body(raw_link, 'forget_run') and 'window_set(false, "answer: No' in body(raw_link, 'prompt_settle')
assert 'window_set(true, "auto at start: no bond is stored")' in body(raw_link, 'ble_init_task') and 'BLE start: %u bond%s stored' in raw_link
assert 'pairing window closed: expired' in body(raw_link, 'ble_link_task') and 's_window_close_logged' in body(link, 'ble_link_task')
assert 'security request %s: window %s, peer %s' in raw_link and 'NOT sent' in raw_link, 'the request, sent or not, says why'
# Item 4a: advertising goes on while the window is open (same locked step as the other hold)
assert re.search(r'ble_enc_hold_update\(&s_ble, &s_pair, now, s_prompt\.active\);[^\n]*\n\s*ble_adv_hold_update\(&s_ble, &s_pair, now\);[^\n]*\n\s*const ble_event_t due = ble_poll\(&s_ble, now\);', body(link, 'ble_link_task')), 'both holds, then the poll, under one lock'
assert not re.search(r'ble_store_(write|delete|clear)', pb), 'a read, nothing else'
assert 'ble_security_request_wanted' in text('ble_state.h') and 'ble_security_request_wanted' in code('ble_state.c')
# Fix A (2026-11-09): the 3 s encryption deadline waits while the window is open and the link is SECURING (and while a number is on show), in the same step as the
# poll, under the lock. The window (120 s) and the 30 s cap on a shown number bound it.
task_a = body(link, 'ble_link_task')
assert re.search(r'ble_enc_hold_update\(&s_ble, &s_pair, now, s_prompt\.active\);\s*ble_adv_hold_update\(&s_ble, &s_pair, now\);\s*const ble_event_t due = ble_poll\(&s_ble, now\);', task_a), 'hold, then poll, under one lock'
assert task_a.index('state_lock()') < task_a.index('ble_enc_hold_update(') < task_a.index('ble_poll(&s_ble, now)') < task_a.index('state_unlock()')
# Review M1 (2026-11-10): the window's hold is capped per link in the pure module (BLE_ENC_HOLD_MAX_MS, 30 s from the connect); the glue passes the number-on-screen flag only.
assert 'BLE_ENC_HOLD_MAX_MS' in text('ble_state.h') and 'age < BLE_ENC_HOLD_MAX_MS' in code('ble_state.c'), 'the hold of an open window is capped'
assert 'prompt_settle(-1)' in task_a, 'the 30 s cap on a shown number is still asked for every tick'
# Auto-open (2026-11-09): with phone mode on and NO bond stored, the window opens by itself once the link is started, from ble_init_task (the phone mode runs only
# after an unplug, so this is each time the BLE link comes up). The pure rule is ble_pair_auto_open_wanted; the window is the ordinary one.
init_a = body(link, 'ble_init_task')
assert 'if (ble_pair_auto_open_wanted(s_started, bonds_at_start)) {' in init_a and 'window_set(true, "")' in init_a
assert init_a.index('s_started = true;') < init_a.index('ble_link_bond_count()') < init_a.index('ble_pair_auto_open_wanted(') < init_a.index('ble_link_task,'), 'the bond count is read once the link is started (it asks the running host)'
assert code('ble_link.c').count('ble_pair_auto_open_wanted(') == 1 and 'ble_link_pairing_window(true)' not in code('ble_link.c'), 'one auto-open call at the start; the re-arm is ble_pair_rearm_wanted at a connect; the UI [pair] is the other opener'
assert 'ble_pair_auto_open_wanted' in text('ble_state.h') and 'ble_enc_hold_update' in text('ble_state.h')
uuids = {}
for line in (fw / 'test/vectors/ble_status.txt').read_text(encoding='utf-8').replace('\r\n', '\n').split('\n'):
    f = line.split('\t')
    if f[0] == 'uuid':
        uuids[f[1]] = f[3]
assert set(uuids) == {'service', 'psm', 'status', 'hint'}
for role, ident in (('service', 'UUID_SVC'), ('psm', 'UUID_PSM'), ('status', 'UUID_STATUS'), ('hint', 'UUID_HINT')):
    m = re.search(r'static const ble_uuid128_t ' + ident + r' = BLE_UUID128_INIT\(([^)]*)\);', link)
    assert m, ident
    assert ''.join(f'{int(b, 16):02x}' for b in m[1].split(',')) == uuids[role], f'{ident} is the {role} UUID of the vector file, last byte first'
table = re.search(r'static const struct ble_gatt_svc_def GATT_SVCS\[\] = \{(.*?)\n\};', link, re.S)
assert table, 'one GATT table'
chrs = {}
for ident, flags in re.findall(r'\.uuid = &(UUID_\w+)\.u,[^{}]*?\.flags = ([^,}]*)', table[1], re.S):
    chrs[ident] = {x.strip() for x in flags.split('|')}
assert chrs['UUID_PSM'] == {'BLE_GATT_CHR_F_READ'}, 'the PSM is readable without encryption (spec section 5)'
assert chrs['UUID_STATUS'] == {'BLE_GATT_CHR_F_READ', 'BLE_GATT_CHR_F_READ_ENC', 'BLE_GATT_CHR_F_WRITE', 'BLE_GATT_CHR_F_WRITE_ENC', 'BLE_GATT_CHR_F_NOTIFY'}
assert chrs['UUID_HINT'] == {'BLE_GATT_CHR_F_WRITE', 'BLE_GATT_CHR_F_WRITE_ENC'}, 'the home hint is write only, and only encrypted'
assert table[1].count('.uuid = &UUID_SVC.u') == 1 and 'BLE_GATT_SVC_TYPE_PRIMARY' in table[1]
chr_cb = body(link, 'chr_access')
# a write is taken from a peer that passes the same rule as the channel (encrypted, bonded, authenticated, long key; the dev flag waives it), and the
# value is checked by the pure functions that test_ble_state.c runs against the vector file
assert 'ble_accept_check(&sec, DEV_UNBONDED, false) != BLE_ACCEPT_OK' in chr_cb and 'BLE_ATT_ERR_INSUFFICIENT_AUTHEN' in chr_cb
assert 'ble_hint_check(' in chr_cb and 'ble_relay_status_check(' in chr_cb
init_b = body(link, 'ble_init_task')
assert init_b.index('ble_gatts_count_cfg(GATT_SVCS)') < init_b.index('ble_gatts_add_svcs(GATT_SVCS)') < init_b.index('nimble_port_freertos_init(host_task)'), 'the table is added before the host starts'
assert 'ble_link_home_hint' in text('ble_link.h') and 'ble_link_relay_status' in text('ble_link.h')

# ── 5b. B5: the pairing window, the number on the screen, Forget ────────────────────────────────────────────────────────────────────────
# The offer: only inside the window, only the numeric comparison; anything else is refused and the link is ended. The number is kept under the lock for the
# screen (ble_link_pair_prompt) and never logged.
pk = re.search(r'case BLE_GAP_EVENT_PASSKEY_ACTION:(.*?)(?=\n    case |\n    default:)', gap, re.S)
assert pk, 'the passkey event is handled (it fell to default before B5)'
pk = pk[1]
assert 'event->passkey.params.action == BLE_SM_IOACT_NUMCMP' in pk and 'ble_numcmp_request(&s_pair, now_ms()) == BLE_NUMCMP_SHOW' in pk
assert 'ble_set_prompting(&s_ble, true)' in pk and 'event->passkey.params.numcmp' in pk
assert 'terminate_link()' in pk, 'any other kind of passkey request ends the link'
settle = body(link, 'prompt_settle')
assert 'ble_sm_inject_io(' in settle and link.count('ble_sm_inject_io(') == 2, 'one answer path, one refusal at the request'
assert 'io.numcmp_accept = 0' in pk, 'a refused request is answered No'
assert 'ble_prompt_verdict(&s_pair, s_prompt.since_ms, now_ms(), answer)' in settle and 'ble_set_prompting(&s_ble, false)' in settle   # ble_prompt_verdict = ble_numcmp_verdict + the 30 s bound (review M1)
# Review L3 (2026-11-10): a No closes the window EXCEPT with phone mode on and no bond stored (ble_pair_stays_open_after_no, the auto-open's own rule): then it stays
# open to its original end. The bond count is read outside the lock (a call into the host), only after a REJECT (not on the 20 ms tick), and the close that remains is the old one.
assert 'ble_pair_stays_open_after_no(s_started, ble_link_bond_count())' in settle, 'one rule decides whether a No closes the window'
assert re.search(r'if \(close_window\) window_set\(false, ""\)', settle), 'a No with a bond stored (or the end of the window) still closes the window (window_set also switches bonding off)'
assert 'apply_bonding_policy();' in body(link, 'window_set')
assert re.search(r'bool close_window = v == BLE_PAIR_REJECT;\s*if \(close_window && ble_pair_stays_open_after_no\(', settle), 'the count is asked only for a rejection'
assert settle.index('ble_pair_stays_open_after_no(') > settle.index('state_unlock()'), 'the host call is made outside the lock'
assert settle.index('close_window') < settle.index('ble_sm_inject_io('), 'the window is settled before the answer goes to the stack'
assert 'ble_pair_open(' not in settle and 'window_set(true' not in settle and 'ble_link_pairing_window(true)' not in settle, 'a No never opens or extends a window: no loop'
assert 'terminate_link()' in settle, 'a rejected pairing ends the link'
assert 'prompt_settle(-1)' in body(link, 'ble_link_task'), 'the window ending under the number is noticed by the link task'
assert 'prompt_settle(yes ? 1 : 0)' in body(link, 'ble_link_pair_answer')
assert 's_prompt.active = false' in body(link, 'gap_event'), 'a lost link takes the offer with it'
for fn in ('gap_event', 'prompt_settle', 'ble_link_pair_prompt'):
    for call in re.findall(r'ESP_LOG[EWIDV]\((.*?)\);', body(link, fn), re.S):   # literals are blanked: the arguments are what is checked
        assert 'number' not in call and 'numcmp' not in call and 'passkey' not in call, f'{fn} logs the number'
# Forget (review M3): the wipe runs on the NimBLE host task, which owns the bond store. ble_link_forget only posts the event; the handler is the one caller.
fget = body(link, 'forget_run')
assert fget.index('window_set(false, "")') < fget.index('prompt_settle(-1)') < fget.index('ble_store_clear()') < fget.index('terminate_link()'), 'Forget: close the window, drop an offer, wipe the bonds once, end the link'
post_forget = body(link, 'ble_link_forget')
assert 'ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev)' in post_forget and 'ble_store_clear' not in post_forget, 'ble_link_forget posts to the host task and wipes nothing itself'
assert re.search(r'ble_npl_event_init\(&s_forget_ev, forget_run, NULL\);', body(link, 'ble_init_task')), 'the event runs forget_run, set up before the host starts'
assert body(link, 'ble_init_task').index('ble_npl_event_init(&s_forget_ev') < body(link, 'ble_init_task').index('nimble_port_freertos_init(host_task)')
assert link.count('forget_run') == 2, 'the definition and the one event_init'
busy = body(link, 'ble_link_pairing_busy')
assert 'ble_pair_is_open(&s_pair, now_ms())' in busy and 's_prompt.active' in busy
hdr = text('ble_link.h')
for name in ('ble_link_pair_prompt', 'ble_link_pair_answer', 'ble_link_pairing_busy', 'ble_link_forget', 'ble_link_bond_count', 'ble_link_pairing_left_ms'):
    assert hdr.count(name + '(') == 2, f'{name}: declared and stubbed'
glue_busy = body(code('mode_glue.c'), 'mode_switch_busy')
assert 'ble_link_pairing_busy()' in glue_busy and 'MS_BUSY_PAIRING' in glue_busy, 'an open pairing window holds the mode switch'
assert '#include "ble_link.h"' in text('mode_glue.c')

# ── 5c. review fixes after B5 (M1, M2, M4 and the Low items) ──────────────────────────────────────────────────────────────────────────
gapb = body(link, 'gap_event')
# M4: three one-token facts that the earlier checks only held in pieces. Each is pinned as a whole line so that changing one token fails here.
assert 'io.numcmp_accept = v == BLE_PAIR_ACCEPT;' in settle, 'the answer to the stack is Yes only for an ACCEPT verdict'
chr_b = body(link, 'chr_access')
gate = 'if (ble_accept_check(&sec, DEV_UNBONDED, false) != BLE_ACCEPT_OK) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;'
assert gate in chr_b, 'a write needs the channel rule, and is refused with "insufficient authentication"'
assert chr_b.index('(void)link_sec(conn, &sec);') < chr_b.index(gate) < chr_b.index('atomic_store(&s_home_hint') and chr_b.index(gate) < chr_b.index('atomic_store(&s_relay_status'), 'the gate comes before either store'
assert chr_b.count('atomic_store(') == 2, 'two stores, both behind the gate'
assert 'const bool show = numcmp && !s_prompt.active && ble_numcmp_request(&s_pair, now_ms()) == BLE_NUMCMP_SHOW;' in gapb, 'show: a numeric comparison AND no offer on show AND the window open'
# M1: a failed encryption takes the offer with it; a shown number is withdrawn after BLE_PROMPT_MAX_MS
enc = re.search(r'case BLE_GAP_EVENT_ENC_CHANGE:(.*?)\n    case BLE_GAP_EVENT_REPEAT_PAIRING', gapb, re.S)
assert enc, 'ENC_CHANGE'
enc = enc[1]
failed = re.search(r'if \(event->enc_change\.status != 0\) \{(.*?)\n        \}', enc, re.S)
assert failed, 'a failed encryption has its own branch'
failed = failed[1]
assert 's_prompt.active = false;' in failed and 's_prompt.number = 0;' in failed and 'ble_set_prompting(&s_ble, false);' in failed, 'the offer and the deadline hold are cleared'
assert failed.index('state_lock()') < failed.index('s_prompt.active = false;') < failed.index('ble_set_prompting(&s_ble, false);') < failed.index('state_unlock()') < failed.index('terminate_link()'), 'under the lock, then the link is ended'
assert 'ble_prompt_verdict(&s_pair, s_prompt.since_ms, now_ms(), answer)' in settle and 'ble_numcmp_verdict(' not in settle, 'the bounded verdict'
assert 's_prompt.since_ms = now_ms();' in gapb, 'the time the number went on show'
assert 'since_ms' in raw_link[raw_link.index('static struct { bool active'):raw_link.index('static struct { bool active') + 160]
# M2: Just Works must not fill the two bond slots
assert 'ble_bond_unwanted(&sec)' in enc and 'ble_store_util_delete_peer(&d.peer_id_addr)' in enc, 'a bond that is not authenticated and long-keyed is deleted'
assert enc.index('ble_bond_unwanted(&sec)') < enc.index('ble_store_util_delete_peer(&d.peer_id_addr)') < enc.index('terminate_link()', enc.index('ble_store_util_delete_peer')) < enc.index('note_security('), 'delete it, end the link, and only otherwise judge the link trusted'
assert re.search(r'ble_gap_conn_find\(event->enc_change\.conn_handle, &d\) == 0', enc)
# Low: the window closing in prompt_settle re-applies the bonding switch; a lost link forgets what the phone wrote; the inject result is logged
assert re.search(r'if \(close_window\) window_set\(false, ', settle) and settle.index('window_set(false') < settle.index('ble_sm_inject_io('), 'a closed window switches bonding off at once (L3: not when it stays open)'
disc = re.search(r'case BLE_GAP_EVENT_DISCONNECT:(.*?)\n    case BLE_GAP_EVENT_ENC_CHANGE', gapb, re.S)[1]
assert 'atomic_store(&s_relay_status, 0);' in disc and 'atomic_store(&s_home_hint, 0);' in disc, 'what the phone wrote is forgotten with the link'
assert 'const int irc = ble_sm_inject_io(conn, &io);' in settle and re.search(r'ESP_LOG[IW]\(TAG, "pairing [^"]*rc=%d', raw_link[raw_link.index('static void prompt_settle'):raw_link.index('bool ble_link_pair_prompt')]), 'the stack\'s answer is logged'

# ── 5. the link task: no flash operation, the key read once ─────────────────────────────────────────
# B4e (review H1): the bond store never evicts; a bond is made only inside the pairing window; a repeat pairing never deletes a bond outside it
assert 'ble_store_util_status_rr' not in link and 'ble_gap_unpair' not in link and 'ble_store_delete' not in link, 'no eviction anywhere'
# B5: the one deliberate wipe of the bonds is the user's Forget row; nothing else in the file clears the store, and the link task never calls it (it writes flash)
assert link.count('ble_store_clear(') == 1 and 'ble_store_clear()' in body(link, 'forget_run'), 'one wipe, in the host-task handler forget_run'
assert link.count('ble_link_forget(') == 1, 'only the definition: the link task does not call it'
assert link.count('forget_run') == 2 and 'forget_run(' not in re.sub(r'static void forget_run\(struct ble_npl_event \*ev\)', '', link), 'nothing calls the handler directly: it is reached through the event queue only'
assert 'ble_hs_cfg.store_status_cb = store_status;' in link
st = body(link, 'store_status')
assert 'BLE_STORE_EVENT_OVERFLOW' in st and 'BLE_STORE_EVENT_FULL' in st and st.count('return BLE_HS_ENOMEM;') == 1 and 'return 0' not in st, 'full or overflowing: refuse the new bond'
assert 'ble_hs_cfg.sm_bonding = 0;' in link and 'ble_hs_cfg.sm_bonding = 1' not in link, 'bonding starts closed'
pol = body(link, 'apply_bonding_policy')
assert 'ble_pair_is_open(&s_pair, now_ms())' in pol and 'ble_hs_cfg.sm_bonding = open ? 1 : 0;' in pol and 'sm_our_key_dist' in pol, 'bonding follows the window, and only the window'
assert len(re.findall(r'sm_bonding\s*=', link)) == 2, 'sm_bonding is written at init (closed) and by the policy, nowhere else'
gap = body(link, 'gap_event')
assert re.search(r'case BLE_GAP_EVENT_REPEAT_PAIRING:.*?ble_pair_is_open\(&s_pair, now_ms\(\)\).*?BLE_GAP_REPEAT_PAIRING_RETRY.*?BLE_GAP_REPEAT_PAIRING_IGNORE', gap, re.S)
assert 'window_set(open, ' in body(link, 'ble_link_pairing_window') and 'ble_pair_open(&s_pair, now_ms())' in body(link, 'window_set') and 'ble_pair_close(&s_pair)' in body(link, 'window_set')
assert 'ble_pair_init(&s_pair)' in raw_link
# review H2 and M1: the abuse bounds
task_b = body(link, 'ble_link_task')
assert 'due == BLE_EV_IDLE_TIMEOUT' in task_b
assert 'ble_churn_exceeded(&s_ble)' in task_b and 'BLE_COOLDOWN_MS' in task_b, 'a link that burns channels is ended and the dial waits'
assert 'ble_refused_wait_ms(trusted, &s_refused_ladder, esp_random())' in task_b, 'only a trusted peer climbs the refused ladder'
# review L3: the deadline and its effect are one step under the lock; the glue acts only when the state really changed
assert re.search(r'const ble_event_t due = ble_poll\(&s_ble, now\);\s*const bool changed = [^;]*ble_post\(&s_ble, due, now\)', task_b, re.S), 'poll and post under one lock'
assert 'post(BLE_EV_ENC_TIMEOUT)' not in task_b and 'post(BLE_EV_ADV_TIMEOUT)' not in task_b, 'no second, unlocked post after the decision'
# review L4: a receive buffer the stack did not take is freed
arm = body(link, 'rx_arm')
assert re.search(r'rc == BLE_HS_EBUSY \|\| rc == BLE_HS_EINVAL\)\s*os_mbuf_free_chain\(m\);', arm), 'EBUSY and EINVAL leave the mbuf with us'
# review L5: start-up and the abandonment are decided by one compare-and-swap, like net_link.c
init = body(link, 'ble_init_task')
assert 'atomic_compare_exchange_strong(&b->phase, &expected, PHASE_COMMITTED)' in init and init.index('PHASE_COMMITTED') < init.index('ble_link_task')
start_fn = body(link, 'ble_link_start')
assert 'atomic_compare_exchange_strong(&b->phase, &expected, PHASE_ABANDONED)' in start_fn
assert 's_abandoned' not in link, 'the old flag (a race) is gone'
# review M2: the development marker is in a development image and in no other
assert raw_link.count('BLE_DEV_UNBONDED_IMAGE') == 1 and re.search(r'#ifdef BLE_DEV_ALLOW_UNBONDED\n\s*ESP_LOGW\(TAG, "DEVELOPMENT BUILD BLE_DEV_UNBONDED_IMAGE', raw_link)
for path in sorted(main.rglob('*')):
    if path.suffix in ('.c', '.h') and path.name != 'ble_link.c':
        assert 'BLE_DEV_UNBONDED_IMAGE' not in path.read_text(errors='replace'), f'{path.name} carries the development marker'

FLASH = re.compile(r'\b(nvs_\w+|config_\w+|esp_ota_\w+|esp_partition_\w+|esp_flash_\w+|spi_flash_\w+|fw_update_\w+)\(')
for fn in ('ble_link_task', 'handshake', 'advertise', 'terminate_link', 'port_send', 'port_disconnect', 'port_rearm', 'port_peer_mtu', 'log_stacks',
           'x_read', 'x_write', 'x_is_up', 'chr_access', 'prompt_settle', 'post', 'current', 'link_sec', 'note_security', 'store_status', 'apply_bonding_policy', 'rx_arm'):
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
assert 'cable_link_drain_logs(DRAIN_LINES)' in task and 'ble_refused_wait_ms(trusted, &s_refused_ladder, esp_random())' in task, 'the refused ladder of net_state.h, for a trusted peer only (ble_state.c)'
assert 'ble_gap_adv_stop()' in task and 'terminate_link()' in task
assert 'tls->close(tls)' in task, 'a finished session is closed before the next handshake'
tls_h = code('cable_xport_tls.c')
assert 'inner->kind == CABLE_XPORT_USB' in tls_h, 'the TLS wrapper refuses USB, and takes any other inner transport'

# ── 6b. B4d: advertising does not stay stopped; the log lines the e2e script reads ──────────────────────
# The retry out of NOT_FOUND comes from ble_poll (BLE_RETRY_MS, `adv_retry_ms`), and any screen wake restarts it at once. Neither touches flash.
assert define(text('ble_state.h'), 'BLE_RETRY_MS') == vectors['adv_retry_ms'] == 120000
assert 'changed && due == BLE_EV_START' in task, 'the retry is a START posted by the deadline step (B4e)'
assert 'const uint32_t wake = display_wake_seq();' in task and 'wake != seen_wake' in task, 'a screen wake is seen by its counter'
assert re.search(r'if \(wake != seen_wake\) \{\s*seen_wake = wake;\s*if \(st == BLE_ST_NOT_FOUND\) \{\s*post\(BLE_EV_START\);', task), 'a wake posts START only from NOT_FOUND'
assert task.index('wake != seen_wake') < task.index('advertise();'), 'the wake is looked at before the advertising keeper, so it advertises in the same tick'
assert '#include "ui/display.h"' in raw_link and 'display_wake_seq' not in code('ble_state.c')
# e2e.ps1 (phone-bridge/tools, branch bridge-daemon) matches these three lines of the dial log; the tag is ble_link
assert re.search(r'ESP_LOGI\(TAG, "advertising name=%s"', raw_link), 'advertise start'
assert re.search(r'ESP_LOGI\(TAG, "channel up psm=0x%04x mtu=%u"', raw_link), 'channel up'
assert re.search(r'ESP_LOGI\(TAG, "link up #%u handshake_ms=%d boot_ms=%d"', raw_link), 'TLS-PSK up'
assert 'static const char *TAG = "ble_link";' in raw_link

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
