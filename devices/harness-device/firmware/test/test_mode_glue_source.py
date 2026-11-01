"""W15: source rules for the glue of the automatic mode switch, checked without a toolchain. What the host tests (test_mode_glue.c,
test_usb_probe.c, test_mode_record.c, test_mode_config.c) cannot reach is the IDF side: the calls into the drivers, the UI and app_main. These are
the rules that keep a dial able to get back to USB.

  1. app_main: the boot decision comes after the factory-reset countdown and decides whether net_link_start() runs at all; the focus is restored and
     the task started AFTER the link, with the mode that really started (the radio may have failed: then USB).
  2. mode_glue.c: the SOF flag is read once, no earlier than MS_SOF_SETTLE_MS; the RTC record is trusted only for esp_reset_reason() == ESP_RST_SW;
     the task has an internal stack (it writes NVS and installs the USB driver); the reboot goes through the one sequence (mode_glue_core.c) in
     the order log, seal, store, restart; a probe that would not install is retried; MS_FOCUS_MAX equals ID_MAX.
  3. No target, no switch: nothing in the firmware can set wl_mode to 2 (Bluetooth has no transport) and nothing writes stay_wl but the Link screen's toggle, which turns it off as well as on (the Settings
     row is task W16), so no dial ends up in a wireless mode it cannot leave. boot_usb is written by the glue only.
  4. usb_probe.c: the only other installer of the USB driver besides the cable transport; small rings; hello from usb_probe_core; no JSON library;
     called only from the mode switch task; the probe is never on while the dial runs on USB.
  5. The UI predicate: every row of the busy list (MS section 3) is in ui_mode_switch_busy, under the display lock; the notice is an overlay and not a
     view; the model list, the pet upload and the update are counted by mode_switch_busy.
  6. last_words: the reason of a mode switch survives the reset and is used once.
  7. The build and the test run know the new files.
  8. Review fixes (2026-10-31): the restart op freezes the NVS writers and looks at the busy gate last; the one cross-task 64-bit timestamp is atomic;
     the probe's drain is bounded; the welcome scanner's depth limit.
  9. The panic loop: the exact sets of reset reasons, the order of the crash record's reads and writes at boot, the 10-minute clear.
"""
from pathlib import Path
import os
import re

fw = Path(__file__).resolve().parent / '..'
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


# ── 1. app_main ─────────────────────────────────────────────────────────────────────────────────────
app = code('app_main.c')
app_main = body(app, 'app_main')
i_countdown = app_main.index('reset_countdown()')
i_boot = app_main.index('mode_glue_boot()')
i_net = app_main.index('net_link_start()')
i_start_link = app_main.index('cable_client_start_with(')
i_focus = app_main.index('mode_glue_restore_focus()')
i_task = app_main.index('mode_glue_start(')
assert i_countdown < i_boot < i_net < i_start_link < i_focus < i_task, 'order: factory reset, boot decision, radio, link, focus, task'
assert re.search(r'const bool wifi_mode = mode_glue_boot\(\) == MS_MODE_WIFI && net_link_start\(\);', app_main), \
    'net_link_start() runs only when the boot decision is Wi-Fi (a dial that decided USB never starts the radio)'
assert re.search(r'mode_glue_start\(wifi_mode \? MS_MODE_WIFI : MS_MODE_USB\);', app_main), 'the task gets the mode that really started'
assert app.count('net_link_start(') == 1 and app.count('mode_glue_boot(') == 1 and app.count('mode_glue_start(') == 1
# the gallery boot (a diagnostic image) has no mode switch
assert 'mode_glue' not in body(app, 'gallery_main')
# nothing but app_main calls the three entry points
for path in main.rglob('*.c'):
    if path.name in ('app_main.c', 'mode_glue.c'):
        continue
    src = path.read_text(errors='replace')
    assert not re.search(r'\bmode_glue_(boot|start|restore_focus)\(', src), f'{path.name} calls the boot entry points'

# ── 2. mode_glue.c ──────────────────────────────────────────────────────────────────────────────────
glue = code('mode_glue.c')
raw_glue = text('mode_glue.c')
boot = body(glue, 'mode_glue_boot')
assert boot.count('usb_serial_jtag_is_connected()') == 1, 'the SOF flag is read once at boot'
assert boot.index('MS_SOF_SETTLE_MS') < boot.index('usb_serial_jtag_is_connected()'), 'the 20 ms settle comes before the read'
assert 'esp_reset_reason() == ESP_RST_SW' in boot and 'mode_record_boot(' in boot, 'the record is believed only after a software reset'
assert boot.index('mode_record_boot(') < boot.index('mg_boot('), 'the record is read before the decision'
# the panic loop at boot: the crash record is read (with the class of the reset) before the decision, sealed right after it, and the reason is logged
assert 'mode_record_crash_boot(reset_class(esp_reset_reason()), &in.crashes)' in boot and boot.index('mode_record_crash_boot(') < boot.index('mg_boot(')
assert 'in.crash_trip = mode_record_crash_boot(' in boot, 'the answer of the crash record is the decision input'
assert 'mode_record_crash_seal(s_boot.crashes, s_boot.mode)' in boot and boot.index('mg_boot(') < boot.index('mode_record_crash_seal('), 'sealed with the decided mode'
assert 'Wi-Fi mode crashed %u times; started in USB' in raw_glue
assert 's_boot.crash_usb' in boot
assert 'config_load_wl_effective()' in boot and 'config_load_stay_wl()' in boot and 'config_load_boot_usb()' in boot
assert 'config_save_boot_usb(s_boot.boot_usb_new)' in boot and 's_boot.write_boot_usb' in boot, 'boot_usb is rewritten only when the plan says so'
assert re.search(r'_Static_assert\(MS_FOCUS_MAX == ID_MAX', raw_glue), 'the record holds an agent id: same size as cable_client.h'
assert re.search(r'#define ID_MAX\s+48\b', text('cable_client.h')) and re.search(r'#define MS_FOCUS_MAX\s+48\b', text('mode_switch.h'))
# the task: internal stack, big enough, one create
assert re.search(r'xTaskCreate\(mode_sw_task, "mode_sw", TASK_STACK, NULL, TASK_PRIO, NULL\)', raw_glue), 'internal stack (xTaskCreate)'
assert 'task_create_psram' not in glue
assert int(re.search(r'#define TASK_STACK (\d+)', glue)[1]) >= 4096
# the sequence: the ops table is the one place the reboot is spelled, and the core fixes the order (test_mode_glue.c holds it)
ops = re.search(r'static const mg_ops_t OPS = \{(.*?)\};', glue, re.S)[1]
for field in ('probe', 'notice', 'clear_flaps', 'log_reason', 'seal', 'store_boot_usb', 'restart', 'clear_crashes', 'undo'):
    assert re.search(r'\.' + field + r' = op_\w+', ops), field
assert glue.count('esp_restart()') == 1 and 'esp_restart()' in body(glue, 'op_restart')
assert 'last_words_note_reason(MS_REASON_SWITCH)' in body(glue, 'op_log_reason')
assert 'mode_record_save(focus, flaps)' in body(glue, 'op_seal') and 'ui_mode_switch_busy(focus' in body(glue, 'op_seal')
assert 'config_save_boot_usb(boot_usb)' in body(glue, 'op_store_boot_usb')
core = code('mode_glue_core.c')
step = body(core, 'mg_step')
order = [step.index(x) for x in ('ops->log_reason(', 'ops->seal(', 'ops->store_boot_usb(', 'ops->restart(')]
assert order == sorted(order), 'log, seal, store, restart'
# a refused restart is undone and the core goes on: done is cleared, so the sequence runs again at a later step
assert re.search(r'if \(ops->restart && !ops->restart\(ops->ctx, out\.target\)\) \{\s*if \(ops->undo\) ops->undo\(ops->ctx, g->ms\.mode, g->ms\.flaps\);\s*g->done = false;', step), \
    'a refused restart: undo, then done = false'
assert 'g->ms.mode == MS_MODE_WIFI' in step and 'MS_CRASH_CLEAR_MS' in step and 'ops->clear_crashes(' in step, 'ten minutes up in Wi-Fi clear the crash count; USB time does not'
# a probe that did not install is tried again: a dial that cannot probe cannot come back to USB
task = body(glue, 'mode_sw_task')
assert 'PROBE_RETRY_MS' in task and 'usb_probe_running()' in task and 'usb_probe_start(' in task
assert 'usb_probe_poll(' in task and task.index('usb_probe_poll(') > task.index('usb_probe_start(')
# the probe is asked for only through the module's answer, and never while the dial runs on USB
assert 'usb_probe_start(' in body(glue, 'op_probe') and 'usb_probe_stop()' in body(glue, 'op_probe')
# the restart op (review finding 2): the NVS writers are frozen, THEN the busy gate is looked at (hard and soft rows, the step's own predicate), THEN
# esp_restart() with nothing in between; a refusal thaws and returns false. The undo op puts boot_usb and the record back.
assert re.search(r'static bool op_restart\(', glue)
rs = body(glue, 'op_restart')
assert rs.index('usb_probe_stop()') < rs.index('store_gate_freeze(') < rs.index('mode_switch_blocked()') < rs.index('esp_restart()')
assert re.search(r'if \(mode_switch_blocked\(\)\) \{[^}]*store_gate_thaw\(\);[^}]*return false;\s*\}\s*esp_restart\(\);', rs), 'nothing between the last look and the restart; a refusal thaws'
assert rs.count('return false;') == 2 and 'RESTART_FREEZE_MS' in rs
undo = body(glue, 'op_undo')
assert 'config_save_boot_usb(running == MS_MODE_USB)' in undo and 'mode_record_save(NULL, flaps)' in undo and 'last_words_note_reason("")' in undo
assert 'mode_record_crash_clear()' in body(glue, 'op_clear_crashes')
gate = text('store_gate.h')
assert 'store_gate_freeze' in gate.split('#else')[1] and 'store_gate_thaw' in gate.split('#else')[1], 'the build without DEVICE_BATTERY_POWEROFF has the calls too'
sample = body(glue, 'sample')
assert 'usb_probe_last_welcome(' in sample and sample.index('s_running == MS_MODE_USB') < sample.index('usb_probe_last_welcome('), 'welcomes are read only in a wireless mode'
# the clock handed to the module is wrap-safe
assert 'now - (uint32_t)((uint32_t)now - last32)' in sample
# the mode the module sees is the effective one
assert 'config_load_wl_effective()' in body(glue, 'mode_glue_start')
# no BLE anywhere in the glue: no transport exists for it
for name in ('mode_glue.c', 'mode_glue_core.c', 'app_main.c', 'usb_probe.c'):
    assert 'MS_MODE_BLE' not in code(name) and 'MS_WL_BLE' not in code(name), f'{name} names Bluetooth'
assert 'MS_WL_BLE' not in code('config_store.c') and 'MS_MODE_BLE' not in code('config_store.c')

# ── 3. no way to strand a dial ──────────────────────────────────────────────────────────────────────
store = code('config_store.c')
assert re.search(r'config_net_err_t config_net_check_mode\(long mode\) \{ return \(mode == 0 \|\| mode == 1\) \? CFG_NET_OK : CFG_NET_E_MODE; \}', store), \
    'wl_mode 2 (Bluetooth) cannot be stored: net.mode and net.set refuse it'
assert 'mode != 0 && mode != 1' in body(store, 'config_net_check') and 'mode != 0 && mode != 1' in body(store, 'config_net_set_mode')
assert 'out->mode = mode == 1 ? 1 : 0;' in body(store, 'config_net_load') and 'out->mode = mode == 1 ? 1 : 0;' in body(store, 'config_net_info'), \
    'whatever byte is stored, only 1 reads as Wi-Fi'
assert 'info.saved && info.mode == 1' in body(store, 'config_load_wl_effective')
assert 'return ok && v == 1;' in body(store, 'flag_load')
for path in main.rglob('*.c'):
    src = path.read_text(errors='replace')
    if path.name != 'config_store.c':
        # W16: the one writer is the Link screen's toggle (ui_habitat.c link_work), which can turn it off again; test_link_screen.py holds that
        assert 'config_save_stay_wl(' not in src or path.name == 'ui_habitat.c', f'{path.name} writes stay_wl: only the Link screen toggle may (test_link_screen.py)'
        if path.name != 'mode_glue.c':
            assert 'config_save_boot_usb(' not in src, f'{path.name} writes boot_usb'

# ── 4. usb_probe.c ──────────────────────────────────────────────────────────────────────────────────
probe = code('usb_probe.c')
installers = sorted(p.name for p in main.rglob('*.c') if 'usb_serial_jtag_driver_install(' in code(str(p.relative_to(main))))
assert installers == ['cable_xport_usb.c', 'usb_probe.c'], installers
rx, tx = (int(re.search(r'#define PROBE_' + n + r'_BUF (\d+)', probe)[1]) for n in ('RX', 'TX'))
assert 64 < rx <= 2048 and 0 < tx <= 1024, 'the driver needs RX > 64 and TX > 0; the probe keeps them small'
assert 'up_core_hello(frame, sizeof frame, CABLE_PROTO_VERSION, CABLE_PRODUCT)' in probe
assert 'cJSON' not in probe and 'cJSON' not in code('usb_probe_core.c') and 'cJSON' not in text('usb_probe_core.h')
assert 'usb_serial_jtag_driver_uninstall()' in body(probe, 'usb_probe_stop')
for path in main.rglob('*.c'):
    if path.name in ('usb_probe.c', 'mode_glue.c'):
        continue
    assert not re.search(r'\busb_probe_(start|stop|poll)\(', path.read_text(errors='replace')), f'{path.name} drives the probe'
assert glue.count('usb_probe_start(') == 2   # op_probe and the retry in the task

# ── 5. the UI predicate ─────────────────────────────────────────────────────────────────────────────
ui = code('ui/habitat/ui_habitat.c')
busy = body(ui, 'ui_mode_switch_busy')
assert busy.index('display_lock()') < busy.index('MS_BUSY_VOICE') and busy.rindex('MS_BUSY_PICKER') < busy.index('display_unlock()')
rows = {
    'MS_BUSY_VOICE': ['s.voice_open', 's.voice_start_pending', 's.voice_waiting', 's.view == VOICE', 'audio_client_active()'],
    'MS_BUSY_DRAFT': ['draft.page.active', 'draft.pending', 's.view == DRAFT', 's.view == DRAFT_OPTIONS'],
    'MS_BUSY_QUESTION': ['question_view(s.view)', 's.q.valid'],
    'MS_BUSY_READER': ['s.view == READER'],
    'MS_BUSY_PICKER': ['case FORM:', 'case SELECTION:', 'case STOP:', 'case MODELS:', 'case MACHINES:', 'case TABS:', 'case OTA:', 'case MESSAGE:'],
}
for bit, needs in rows.items():
    assert bit in busy, bit
    for need in needs:
        assert need in busy, f'{bit}: {need}'
assert 'focus' in busy and 'active()' in busy
# the notice is an overlay: it sets a flag and asks for a redraw, and does not change the view
notice = body(ui, 'ui_mode_notice')
assert 's.mode_notice = on' in notice and 'change()' in notice and 'view(' not in notice
assert 'if (s.mode_notice) render_mode_notice(f);' in ui
assert 'Switching in a moment' in text('ui/habitat/ui_habitat.c')
blocked = body(glue, 'mode_switch_busy')
for need in ('ui_mode_switch_busy(NULL, 0)', 'cable_client_models_waiting()', 'MS_BUSY_MODEL_LIST', 'cable_client_pet_upload_active()', 'MS_BUSY_PET_UPLOAD',
             'fw_update_active()', 'MS_BUSY_FW_UPDATE'):
    assert need in blocked, need
assert re.search(r'#ifndef DEVICE_NO_OTA\s+if \(fw_update_active\(\)\)', blocked), 'the update counts where an update can exist'
assert 'ms_blocked(mode_switch_busy(), display_is_asleep())' in body(glue, 'mode_switch_blocked')
client = code('cable_client.c')
assert '((model_item_t *volatile *)&s_models_out)[0] != NULL' in body(client, 'cable_client_models_waiting')
assert 's_pet_id[0] != 0' in body(client, 'cable_client_pet_upload_active')
daemon = body(client, 'cable_client_last_daemon_byte')
# The one timestamp that crosses tasks is 64 bits on a 32-bit CPU. A plain read can tear, and a "read twice until equal" loop over a plain variable can
# be merged into one read by the compiler. _Atomic makes each load a single indivisible operation (the library routine, a spinlock in newlib/stdatomic.c).
assert re.search(r'static _Atomic int64_t\s+s_last_rx_us;', client), 's_last_rx_us is atomic'
assert 'atomic_load(&s_last_rx_us)' in daemon and 'tries' not in daemon and 'for (' not in daemon, 'one atomic load, no retry loop to merge'

# the probe's drain is bounded (review finding 3) and the scanner's depth limit is the welcome's, not 16 (finding 5)
poll = body(probe, 'usb_probe_poll')
assert 'up_core_pump(&s_core, port_read, NULL, wait_ms, now_ms)' in poll and 'vTaskDelay(1)' in poll and 'while' not in poll and 'usb_serial_jtag_read_bytes(' not in poll
assert 'usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(wait_ms))' in body(probe, 'port_read')
core_src = code('usb_probe_core.c')
assert re.search(r'#define MAX_DEPTH 6\b', core_src), 'depth 6: a welcome nests three levels at the most'
assert re.search(r'#define UP_DRAIN_CHUNKS (\d+)u', text('usb_probe_core.h')) and int(re.search(r'#define UP_DRAIN_CHUNKS (\d+)u', text('usb_probe_core.h'))[1]) <= 16
pump = body(core_src, 'up_core_pump')
assert 'UP_DRAIN_CHUNKS' in pump and pump.count('rd(ctx') == 2

# ── 9. the panic loop: the exact reset sets ─────────────────────────────────────────────────────────
rc = body(glue, 'reset_class')
groups = {}
for labels, cls in re.findall(r'((?:(?:case ESP_RST_\w+|default):\s*)+)return (MS_RESET_\w+);', rc):
    groups.setdefault(cls, set()).update(re.findall(r'ESP_RST_\w+|default', labels))
assert groups == {
    'MS_RESET_POWERON': {'ESP_RST_POWERON'},
    'MS_RESET_HOLD': {'ESP_RST_SW', 'ESP_RST_DEEPSLEEP', 'ESP_RST_EXT', 'ESP_RST_USB', 'ESP_RST_JTAG'},
    'MS_RESET_CRASH': {'ESP_RST_PANIC', 'ESP_RST_INT_WDT', 'ESP_RST_TASK_WDT', 'ESP_RST_WDT', 'ESP_RST_BROWNOUT', 'ESP_RST_UNKNOWN', 'ESP_RST_SDIO',
                       'ESP_RST_EFUSE', 'ESP_RST_PWR_GLITCH', 'ESP_RST_CPU_LOCKUP', 'default'},
}, groups
# every reason the SDK knows is named above, so a new one is a decision (checked when IDF_PATH is set, as run.sh does for the other SDK tests)
idf = os.environ.get('IDF_PATH')
if idf and (Path(idf) / 'components/esp_system/include/esp_system.h').exists():
    sdk = (Path(idf) / 'components/esp_system/include/esp_system.h').read_text()
    enum = re.search(r'typedef enum \{(.*?)\} esp_reset_reason_t;', sdk, re.S)[1]
    known = set(re.findall(r'\bESP_RST_\w+', enum))
    named = set().union(*groups.values()) - {'default'}
    assert known <= named, f'ESP_RST reasons not classified in reset_class(): {sorted(known - named)}'
# the crash record is its own RTC variable with a checksum, and the wrapper writes nothing at boot but the power-on zeroing
rec = code('mode_record.c')
assert re.search(r'static RTC_NOINIT_ATTR ms_crash_t s_crash;', rec)
cb = body(rec, 'mode_record_crash_boot')
assert 'ms_crash_boot(&s_crash, cls, &v)' in cb and 'ms_crash_seal' not in cb and 'cls == MS_RESET_POWERON' in cb
assert 'mg_note_crashes(&s_mg, s_crashes)' in body(glue, 'mode_glue_start') and 'mode_record_crash_seal(s_crashes, s_running)' in body(glue, 'mode_glue_start')
assert 'mode_record_crash_seal(' not in code('app_main.c') and 'mode_record_crash' not in code('usb_probe.c')

# ── 6. last_words ───────────────────────────────────────────────────────────────────────────────────
lw = code('last_words.c')
assert re.search(r'static RTC_NOINIT_ATTR note_t s_note;', lw)
lw_boot = body(lw, 'last_words_boot')
assert 's_reason == ESP_RST_SW' in lw_boot and 'note_check(&s_note)' in lw_boot and 's_note.magic = 0' in lw_boot, 'believed after a software reset only, used once'
assert 'note_check(&n)' in body(lw, 'last_words_note_reason')
assert 's_note_text' in body(lw, 'last_words_report')

# ── 7. the build and the test run ───────────────────────────────────────────────────────────────────
cmake = text('CMakeLists.txt')
for name in ('mode_switch.c', 'mode_record.c', 'mode_glue_core.c', 'mode_glue.c', 'usb_probe_core.c', 'usb_probe.c'):
    assert f'"{name}"' in cmake, name
run = (fw / 'test/run.sh').read_text().replace('\r\n', '\n')
for name in ('test_mode_config', 'test_mode_record', 'test_usb_probe', 'test_mode_glue', 'test_mode_glue_source.py'):
    assert name in run, name

print('mode switch glue: boot order and the radio gate, SOF settle, software-reset record, internal task stack, reboot sequence, probe retry, '
      'no Bluetooth target, no stay_wl writer, one driver installer besides USB, UI busy rows and overlay notice, last_words reason PASS')
