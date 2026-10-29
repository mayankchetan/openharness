"""The BOOT key and the power-on wipe countdown (plan section 16 D3, 16.1 item 3): the wiring that boot_key.c's host test cannot see.

test_boot_key.c proves the state machines. Here: ptt.c runs the machine and carries out its events through the same entry points the
touch gesture uses, the screen toggle on a BOOT hold is gone, and app_main.c no longer wipes the settings the moment BOOT is seen low.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'
ptt = (main / 'ptt.c').read_text()
app = (main / 'app_main.c').read_text()
ui = (main / 'ui/habitat/ui_habitat.c').read_text()
cmake = (main / 'CMakeLists.txt').read_text()
board_c = (main / 'board/board.c').read_text()

# ---- ptt.c ----
task = re.search(r'static void ptt_task\(void \*arg\)\n\{.*?^\}', ptt, re.M | re.S)[0]
assert 'bk_init(&boot_key, gpio_get_level(BSP_BOOT_BUTTON) == 0);' in task, 'a key held through power-on is ignored until released'
assert 'bk_step(&boot_key, now_ms, gpio_get_level(BSP_BOOT_BUTTON) == 0, power_sleep_input_allowed(), ui_voice_can_start)' in task
for needle in ('case BK_TAP:', 'ui_boot_pressed();', 'case BK_HOLD_START:', 'ui_voice_start();', 'bk_stop_poll(&boot_key, now_ms, ui_voice_is_recording())) ui_voice_stop();'):
    assert needle in task, needle
assert 'screen_toggle("BOOT hold")' not in ptt and 'BOOT_LONG_PRESS_MS' not in ptt, 'the BOOT-hold screen toggle is gone'
assert 'if (pwr_key && power_take_pwrkey_tap()) screen_toggle("PWR tap");' in task, 'PWR still toggles the screen'
# the tap interrupts only on a tap event, never on the press edge
press_case = re.search(r'case BK_PRESS:(.*?)break;', task, re.S)[1]
assert 'ui_boot_pressed' not in press_case and 'display_wake' in press_case

# ---- the voice entry points are the touch gesture's ----
start = re.search(r'void ui_voice_start\(void\)\n\{.*?^\}', ui, re.M | re.S)[0]
assert 'dispatch(make_action((hit_t){.action = A_VOICE}))' in start
gesture = re.search(r'ESP_LOGI\("habitat", "focus touch: start voice"\);.*?dispatch\(make_action\(\(hit_t\)\{\.action = A_VOICE\}\)\);', ui, re.S)
assert gesture, 'the focus-face touch start that ui_voice_start mirrors'
assert re.search(r'void ui_voice_stop\(void\)\n\{\n    display_lock\(\);\n    dispatch\(\(action_t\)\{\.kind = A_VOICE_STOP\}\);', ui), 'stop is the gesture tap\'s A_VOICE_STOP'
can = re.search(r'bool ui_voice_can_start\(void\)\n\{.*?^\}', ui, re.M | re.S)[0]
for needle in ('s.view == HOME || s.view == AGENT', 's.connected', '!s.loading', 'audio_client_active()'):
    assert needle in can, needle

# ---- app_main.c: no wipe on the first sight of BOOT; a countdown, then the wipe, then a restart ----
assert app.count('config_clear_all()') == 1
cd = re.search(r'static void reset_countdown\(void\)\n\{.*?^\}', app, re.M | re.S)[0]
assert cd.index('BKR_CANCELLED') < cd.index('config_clear_all()') < cd.index('esp_restart()')
assert 'ui_show_error("Reset settings"' in cd and 'bkr_step(' in cd and 'board_boot_down()' in cd
body = app[app.index('void app_main(void)'):]
assert 'boot_button_held' not in app
assert body.index('ui_set_brightness(config_load_brightness());') < body.index('reset_countdown();') < body.index('ptt_start();'), 'after the glass is up, before the keys are polled'
assert body.count('reset_countdown();') == 1
assert re.search(r'BK_RESET_HOLD_MS\s+5000', (main / 'boot_key.h').read_text())

# ---- build ----
assert '"boot_key.c"' in cmake
assert 'bool board_boot_down(void)' in board_c
print('BOOT key wiring: ptt runs the machine, hold records through the gesture entry points, no screen toggle, wipe needs the 5 s countdown PASS')
