"""The PMIC power-off (DEVICE_BATTERY_POWEROFF): the wiring that the host tests of the sequence and the triggers cannot see.

test_power_policy.c proves the triggers, test_power_sleep_glue.py the glue that asks for them, test_power_off.py the sequence, and
test_debug_cmd_json.c the bench command's wire format. Here: the flag is off by default and needs the sleep flag, every NVS writer goes
through the store gate, the PMIC write is a read-modify-write of REG 0x10 bit 0 and nothing else, the boot report runs after NVS is up, the
bench command exists only with the flag, and an image without the flag carries none of it.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'
cmake = (main / 'CMakeLists.txt').read_text()
power_c = (main / 'board/power.c').read_text()
power_h = (main / 'board/power.h').read_text()
config = (main / 'config_store.c').read_text()
batlog = (main / 'battery_log.c').read_text()
app = (main / 'app_main.c').read_text()
glue = (main / 'power_sleep.c').read_text()
off_c = (main / 'power_off.c').read_text()
off_h = (main / 'power_off.h').read_text()
gate_h = (main / 'store_gate.h').read_text()
dbg_h = (main / 'debug_cmd.h').read_text()
dbg_json = (main / 'debug_cmd_json.c').read_text()
cable_debug = (main / 'cable_debug.c').read_text()
tool = (fw / 'tools/dial_debug.py').read_text()
doc = (fw / 'docs/DEBUG_COMMANDS.md').read_text()

# ---- the flag: default OFF, needs DEVICE_BATTERY_SLEEP, adds only power_off.c ----
block = re.search(r'if\(DEVICE_BATTERY_POWEROFF\)(.*?)\nendif\(\)', cmake, re.S)[1]
assert 'if(NOT DEVICE_BATTERY_SLEEP)' in block and 'message(FATAL_ERROR "DEVICE_BATTERY_POWEROFF needs DEVICE_BATTERY_SLEEP=1' in block
assert 'target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BATTERY_POWEROFF=1)' in block
assert 'target_sources(${COMPONENT_LIB} PRIVATE "power_off.c")' in block
assert 'default OFF' in cmake[cmake.index('# PMIC power-off'):cmake.index('if(DEVICE_BATTERY_POWEROFF)')]
assert '"store_gate.c"' in cmake, 'the gate is always built: the writers call it in every image'

# ---- every NVS writer is bracketed by the gate ----
# 8 writers of the dial's own settings, the 5 Wi-Fi settings writers (config_net_set, _set_key, _set_host, _set_mode, _clear), and the one writer of the
# mode switch's two flags (stay_wl, boot_usb: flag_save, W15)
assert config.count('open_rw(&h)') == 14 and config.count('close_rw(h)') == 14
assert config.count('nvs_open(NS, NVS_READWRITE') == 1, 'one raw read-write open: the helper'
assert 'if (!store_gate_enter()) return ESP_ERR_INVALID_STATE;' in config
blob_set = re.search(r'static bool blob_set\(.*?^\}', batlog, re.M | re.S)[0]
assert blob_set.index('store_gate_enter()') < blob_set.index('NVS_READWRITE') < blob_set.index('nvs_commit(h)') < blob_set.index('store_gate_leave();\n    return e == ESP_OK;')
assert blob_set.count('store_gate_leave()') == 2
# without the flag the gate is an empty inline: the writers are as they were
assert 'static inline bool store_gate_enter(void) { return true; }' in gate_h and 'static inline void store_gate_leave(void) {}' in gate_h
# nothing else in the firmware opens NVS for writing (power_off.c writes its record itself, behind the freeze)
for src in main.rglob('*.c'):
    if src.name in ('config_store.c', 'battery_log.c', 'power_off.c', 'xip_probe.c'):
        continue
    assert 'NVS_READWRITE' not in src.read_text(), str(src)

# ---- the PMIC write: REG 0x10, bit 0 only, read-modify-write, flag-only ----
off_block = power_c[power_c.index('#ifdef DEVICE_BATTERY_POWEROFF'):]
assert '#define AXP_REG_PWROFF        0x10' in off_block and '#define AXP_PWROFF_SOFT       (1 << 0)' in off_block
fn = re.search(r'bool power_pmic_soft_off\(void\)\n\{.*?^\}', off_block, re.M | re.S)[0]
assert 'axp_read(AXP_REG_PWROFF)' in fn and '(v | AXP_PWROFF_SOFT) & ~AXP_PWROFF_RESTART' in fn and 'axp_write(AXP_REG_PWROFF' in fn
assert power_h.count('#ifdef DEVICE_BATTERY_POWEROFF') == 1 and 'bool power_pmic_soft_off(void);' in power_h.split('#ifdef DEVICE_BATTERY_POWEROFF')[1]
assert 'AXP_REG_PWROFF' not in power_c.split('#ifdef DEVICE_BATTERY_POWEROFF')[0], 'REG 0x10 is touched only in the flagged block'

# ---- the sequence in power_off.c is the reviewed one, in order ----
run = re.search(r'static void run\(void\)\n\{.*?^\}', off_c, re.M | re.S)[0]
order = ['fw_update_active()', 'external_power_now("")', 'ESP_LOGW(TAG, "POWEROFF reason=', 'store_gate_freeze(', 'write_record(', 'display_wake()',
         'ui_show_error("Power", "Off - hold PWR")', 'vTaskDelay(pdMS_TO_TICKS(SHOW_MS))', 'external_power_now(" after the screen")', 'power_pmic_soft_off()']
helper = re.search(r'static bool external_power_now\(const char \*when\)\n\{.*?^\}', off_c, re.M | re.S)[0]
assert helper.index('power_pmu_read(') < helper.index('usb_serial_jtag_is_connected()')
pos = [run.index(x) for x in order]
assert pos == sorted(pos), dict(zip(order, pos))
assert '#define SHOW_MS             1000' in off_c
assert 'xTaskCreate(task, "poweroff", 4096' in off_c and 'task_create_psram' not in off_c, 'an internal stack: the task writes NVS'
assert '#ifdef DEVICE_BATTERY_POWEROFF' in off_c and off_c.rstrip().endswith('#endif')

# ---- the glue asks, from the ptt task, only when the flag is on ----
assert glue.count('power_off_request(') == 1 and 'power_off_request((int)why, false)' in glue, 'the policy never forces'
assert re.search(r'#ifdef DEVICE_BATTERY_POWEROFF\n    \{\n.*?pp_off_step\(&s_off, &oin\).*?#endif\n    if \(out.action == PP_ACT_NONE\) return;', glue, re.S), 'asked before the early return'
assert cmake.count('"power_off.c"') == 1, 'power_off.c is listed once, inside the flag block'

# ---- boot: the report runs after NVS is up, and is an empty inline without the flag ----
assert app.index('config_store_init();') < app.index('power_off_boot_report();') < app.index('#ifdef DEVICE_UI_GALLERY\n    gallery_main();')
assert 'static inline void power_off_boot_report(void) {}' in off_h

# ---- the bench command: only with the flag, forced, answered before the dial goes ----
assert re.search(r'typedef enum \{\n    DBG_CMD_NONE, DBG_CMD_SHOT, DBG_CMD_TAP, DBG_CMD_PRESS, DBG_CMD_SWIPE,\n#ifdef DEVICE_BATTERY_POWEROFF\n    DBG_CMD_POWEROFF,\n#endif\n\} dbg_cmd_t;', dbg_h)
assert '#ifdef DEVICE_BATTERY_POWEROFF\n        {"debug.power.off", DBG_CMD_POWEROFF},\n#endif' in dbg_json
po = re.search(r'static void poweroff\(uint16_t id\)\n\{.*?^\}', cable_debug, re.M | re.S)[0]
assert 'power_off_request(POWER_OFF_REASON_DEBUG, true)' in po and po.index('power_off_request(') < po.index('dbg_reply_done(DBG_CMD_POWEROFF, id)')
assert cable_debug.count('#ifdef DEVICE_BATTERY_POWEROFF') == 2
assert 'elif verb == "poweroff":' in tool and '"t": "debug.power.off"' in tool
assert '`debug.power.off`' in doc and 'DEVICE_BATTERY_POWEROFF' in doc.split('debug.power.off')[1][:1500]
print('power off wiring: flag default OFF and needs SLEEP, every NVS writer gated, REG 0x10 read-modify-write, sequence order, bench command only with the flag PASS')
