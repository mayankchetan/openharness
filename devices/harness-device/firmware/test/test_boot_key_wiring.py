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
assert 'bk_init_ex(&boot_key, gpio_get_level(BSP_BOOT_BUTTON) == 0,' in task, 'a key held through power-on is ignored until released'
assert 'bk_step(&boot_key, now_ms, gpio_get_level(BSP_BOOT_BUTTON) == 0, power_sleep_input_allowed(), ui_voice_can_start)' in task
for needle in ('case BK_TAP:', 'ui_boot_pressed();', 'case BK_HOLD_START:', 'ui_voice_start();', 'bk_capture_poll(&boot_key, now_ms, ui_voice_is_recording(), ui_voice_generation())', 'case BK_CAP_SEND:', 'ui_voice_stop();', 'case BK_CAP_CANCEL:', 'ui_voice_route_abort();', 'bk_capture_begin(&boot_key, after)'):
    assert needle in task, needle
assert 'BOOT_LONG_PRESS_MS' not in ptt
assert 'bk_init_ex(&boot_key, gpio_get_level(BSP_BOOT_BUTTON) == 0, !pwr_key);' in task, 'toggle mode exactly where there is no PWR key'
assert task.index('const bool pwr_key') < task.index('bk_init_ex(')
assert re.search(r'case BK_LONG:\n\s+screen_toggle\("BOOT hold"\);', task), 'the old BOOT hold toggle survives for boards without the PMIC'
assert 'if (pwr_key && power_take_pwrkey_tap()) screen_toggle("PWR tap");' in task, 'PWR still toggles the screen'
# the tap interrupts only on a tap event, never on the press edge
press_case = re.search(r'case BK_PRESS:(.*?)break;', task, re.S)[1]
assert 'ui_boot_pressed' not in press_case and 'display_wake' in press_case

# ---- BOOT is the only voice start: touch never starts a capture (owner, 2026-11-11) ----
# ui_voice_start and ui_voice_can_start both go through boot_voice(), the one place that builds a voice purpose from the screen.
boot = re.search(r'static bool boot_voice\(action_t \*out\)\n\{.*?^\}', ui, re.M | re.S)[0]
start = re.search(r'void ui_voice_start\(void\)\n\{.*?^\}', ui, re.M | re.S)[0]
can = re.search(r'bool ui_voice_can_start\(void\)\n\{.*?^\}', ui, re.M | re.S)[0]
assert 'boot_voice(&a)' in start and 'dispatch(a);' in start, 'BOOT dispatches the purpose of the screen it is on'
assert 'a.value == 6 && s.view == DRAFT_OPTIONS) view(DRAFT);' in start, 'Add to message is accepted from the draft, as the touch control was'
assert 'boot_voice(&a)' in can and 's.voice_open' in can and 's.voice_waiting' in can and 'audio_client_active()' in can
assert ui.count('make_action((hit_t){.action = A_VOICE})') == 1, 'the one main-surface voice purpose (Home, agent, Lead) is in boot_voice'
assert 'make_action((hit_t){.action = A_VOICE})' in boot
# Each screen's BOOT purpose, pinned in boot_voice: the gate its touch control had, and the purpose value the worker accepts.
for needle in (
    'if (!s.connected) return false;',
    's.view == HOME || s.view == AGENT', 'if (s.loading || !(active() || swarm_mode())) return false;',
    's.view == QUESTION || s.view == ANSWER_REVIEW', '!s.q.valid || !s.q.supported || s.q.loading || s.q.pending || s.q.error[0] || !q->can_text',
    'if (s.view == QUESTION && q->draft[0]) return false;', 'if (s.view == ANSWER_REVIEW && !q->draft[0]) return false;',
    'a.value = 4; a.revision = s.q.revision; a.dy = s.q.index;', 'COPY(a.id, s.q.agent); copy(a.text, sizeof s.q.token, s.q.token);',
    's.view == FORM', 'form.pending || form.failed || !form.page.active || !form.page.can_query || form.page.busy',
    'a.value = 2; a.revision = form.page.revision; a.dy = (int)form.page.revision;',
    's.view == SELECTION', 'carry.pending || !ht_selection_ready(&selection)', 'a.value = 7; a.dy = (int)selection.revision;',
    's.view == DRAFT || s.view == DRAFT_OPTIONS', '!draft.page.active || draft.pending || draft.failed || draft.page.locked',
    'a.value = s.view == DRAFT ? 5 : 6;', 'else return false;'):
    assert needle in boot, needle
# The worker still accepts each purpose value with the screen check it had (a purpose BOOT builds must not be refused).
for needle in ('if (a.value == 4 && (!question_view(s.view)', 'if (a.value == 2) {\n            if (s.view != FORM', 'if (a.value == 7 && (s.view != SELECTION',
               'if (a.value == 5 || a.value == 6) {\n            if (s.view != DRAFT'):
    assert needle in ui, needle
assert 'focus touch: start voice' not in ui and 'gesture tap: start voice' not in ui
# The touch handler never names A_VOICE (A_VOICE_STOP is the touch's finish and is allowed: \bA_VOICE\b does not match it).
touch = re.search(r'^void habitat_touch\(bool down.*?^\}', ui, re.M | re.S)[0]
assert not re.search(r'\bA_VOICE\b', touch), 'habitat_touch starts voice'
assert 'A_VOICE_STOP' in touch and 'gesture tap: finish voice' in touch, 'a tap or a hold on the voice screen still ends the capture'
# Screen controls that started a capture by touch are dark (no hit), and the draft text and Add to message take no recording.
for dark in ('"[say]", A_FORM_SAY, 0, false)', '"[find]", A_SELECT_FIND, 0, false)', '"[ find ]", A_SELECT_FIND, 0, false)',
             '"[again]",A_QUESTION_SAY,0,false)', '"Add to message", A_DRAFT_APPEND, 0, false)',
             'q->draft[0] ? A_QUESTION_REVIEW : A_QUESTION_SAY,0,q->draft[0] && !s.q.pending)'):
    assert dark in ui, dark
assert 'hold for options' in ui and 'tap to re-speak part' not in ui
assert re.search(r'else if \(a\.kind == A_DRAFT_EDIT \|\| a\.kind == A_DRAFT_APPEND\) \{\n\s+// No recording from a touch', ui), 'a tap on the draft text no longer re-speaks it'
focus = (main / 'ui/habitat/focus.c').read_text()
assert '#define HOLD_TO_TALK "Hold the button to talk"' in focus and '#define HOLD_TALK "Hold the button to talk"' in focus
assert 'Tap to talk' not in focus, 'a resting or relaxing line still tells the user to tap to talk'
assert re.search(r'void ui_voice_stop\(void\)\n\{\n    display_lock\(\);\n    dispatch\(\(action_t\)\{\.kind = A_VOICE_STOP\}\);', ui), 'stop is the tap and hold on the voice screen: A_VOICE_STOP'
# Touch cannot reach the voice purposes: the touch handler does not call boot_voice, and the five touch controls stay dark.
assert 'boot_voice(' not in touch and 'ui_voice_start()' not in touch, 'touch reaches the BOOT voice purposes'

# ---- app_main.c: no wipe on the first sight of BOOT; a countdown, then the wipe, then a restart ----
assert app.count('config_clear_all()') == 1
cd = re.search(r'static void reset_countdown\(void\)\n\{.*?^\}', app, re.M | re.S)[0]
assert cd.index('BKR_CANCELLED') < cd.index('config_clear_all()') < cd.index('esp_restart()')
# after the wipe the restart waits for the release, with no timeout: a restart under a still-held BOOT would count down and wipe again
assert 'while (board_boot_down()) vTaskDelay(pdMS_TO_TICKS(100));' in cd
assert cd.index('config_clear_all()') < cd.index('while (board_boot_down())') < cd.index('esp_restart()')
assert 'i < 100' not in cd
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
