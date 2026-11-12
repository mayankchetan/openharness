"""The menu prompt in ui_habitat.c and ptt.c ("Hold BOOT 2 s for menu"): how the pure machine (menu_prompt.c, test_menu_prompt.c) is wired in.

The Settings list had no touch path from 2026-09-28 (the long hold went to the tabs). User decision 2026-11-11: a pull UP from the bottom edge of the main
face opens a prompt, BOOT held 2 s opens the controls list (Settings is its first row). Read from the source with comments removed:

  * the pull up: starts at y >= 400, passes the swipe threshold, on the main face only; no terminal scroll is begun from that band or while the prompt is up;
  * the prompt owns BOOT: boot_voice refuses (so ui_voice_can_start is false and ui_voice_start starts nothing), a BOOT tap is eaten, ptt.c feeds the key level
    AFTER its own handling so the release that ends the confirming press is eaten first;
  * a swipe down dismisses (before the Inbox pull-down), 10 s close it, leaving the main face closes it, a sleeping screen closes it, a tap does nothing
    (the prompt scene has no hit areas);
  * the confirm opens SETTINGS at its first row and starts no voice;
  * the tap and the hold on the face are as they were (tabs), and the stale "no touch path reaches Settings" comments are gone.
"""
from pathlib import Path
import re

here = Path(__file__).resolve().parent
fw = here / '..'
habitat = fw / 'main/ui/habitat'


def text(path):
    return Path(path).read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def code(path):
    src = re.sub(r'/\*.*?\*/', ' ', text(path), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


raw = text(habitat / 'ui_habitat.c')
ui = code(habitat / 'ui_habitat.c')
ptt = code(fw / 'main/ptt.c')


def function(name, src=ui):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, f'no definition of {name}'
    return match.group(0)


# the pull up
assert '#define MENU_EDGE_Y 400' in ui
touch = function('habitat_touch')
assert re.search(r'else if \(surface && s\.menu\.open\) \{\s*if \(dy > 0\) menu_close\(\);\s*\} else if \(surface && s\.start_y >= MENU_EDGE_Y && dy < 0\) \{\s*menu_open\(\);\s*\} else if \(surface && character\.id == HT_CHARACTER_FOCUS && dy > 0 && notice_unread\(face_id\(\)\)\)', touch), \
    'dismiss and open come before the Inbox pull-down, which stays as it was'
ladder = touch[touch.index('gesture.axis == 1 && abs(dy) > 55 && abs(dy) > abs(dx)'):]
assert ladder.index('menu_open()') < ladder.index('view(INBOX)'), 'the pull up is judged inside the existing swipe threshold (55 px, vertical)'
assert 'if (ui_scroll_reportable() && !(surface && y >= MENU_EDGE_Y)) {' in touch, 'no terminal scroll from the bottom band'
assert '!s.menu.open' in function('ui_scroll_reportable') and 'surface = s.view == HOME || s.view == AGENT' in touch
# tap and hold on the face are unchanged
assert 'tabs_open();' in touch + function('surface_tick') and 'pressed_action.kind == A_PET && !gesture.moved && held >= 650' in function('surface_tick')

# the prompt owns BOOT
boot_voice = function('boot_voice')
assert boot_voice.index('if (s.menu.open) return false;') < boot_voice.index('if (!s.connected)'), 'boot_voice refuses first'
assert 'boot_voice(&a) &&' in function('ui_voice_can_start') and 'if (boot_voice(&a))' in function('ui_voice_start')
pressed = function('ui_boot_pressed')
assert re.search(r'display_lock\(\);\s*if \(ht_menu_eats_tap\(&s\.menu\)\) \{\s*display_unlock\(\);\s*return;\s*\}', pressed), 'a BOOT tap is eaten'
mb = function('ui_menu_boot')
assert 'ht_menu_boot(&s.menu, ms(), down)' in mb and re.search(r'view\(SETTINGS\);\s*s\.offset = 0;', mb), 'the confirm opens the controls list at its first row'
for banned in ('ui_voice_start', 'A_VOICE', 'audio_client', 'boot_voice', 'queue('):
    assert banned not in mb, f'ui_menu_boot must not touch {banned}'
assert 'if (!atomic_load(&menu_live)) return;' in mb, 'cheap while the prompt is down: no lock'
assert 'ht_menu_eats_tap' in function('menu_sync')
# ptt.c: the key level goes to the UI after the machine's event, from the same sample
assert 'const bool boot_down = gpio_get_level(BSP_BOOT_BUTTON) == 0;' in ptt
poll = ptt[ptt.index('switch (bk_step('):]
assert poll.index('ui_boot_pressed();') < poll.index('ui_menu_boot(boot_down && power_sleep_input_allowed());') < poll.index('bk_capture_active(&boot_key)')
assert 'bk_step(&boot_key, now_ms, boot_down,' in ptt
assert 'void ui_menu_boot(bool down);' in text(fw / 'main/ui/ui_screens.h')

# dismiss, timeout, leave
assert 'if (v != HOME && v != AGENT) menu_close();' in function('view')
tick = function('habitat_tick')
assert 'ht_menu_tick(&s.menu, now) || display_is_asleep()' in tick and 'menu_close();' in tick, 'the 10 s, and a sleeping screen'
assert 'ht_menu_next_ms(&s.menu, now)' in function('habitat_next_wake_ms'), 'the bar and the timeout wake the renderer'
# the prompt scene: text, bar, a hint; no hit areas; first in the switch so it always fits
prompt = function('render_menu_prompt')
assert '"Hold BOOT 2 s for menu"' in prompt and 'ht_menu_progress(&s.menu' in prompt and 'swipe down to cancel' in prompt and 'hits' not in prompt and 'control(' not in prompt
assert re.search(r'if \(s\.menu\.open\) render_menu_prompt\(f\);[^\n]*\n\s*else switch \(s\.view\) \{', ui)
# Settings is the first row of the controls list
items = function('settings_item')
assert re.search(r'items\[\] = \{\s*\{"Settings", A_PREFS, 0\},', items), 'Settings first'
# stale comments
assert 'no touch path reaches Settings' not in raw and 'no touch path reaches Settings' not in text(here / 'test_phone_screen.py'), 'the comments tell the truth'
assert 'THE MENU PROMPT' in raw
# built and run
assert '"ui/habitat/menu_prompt.c"' in text(fw / 'main/CMakeLists.txt')
run = text(here / 'run.sh')
assert 'test_menu_prompt' in run and 'test_menu_screen.py' in run
print('menu prompt wiring: pull up at the bottom edge, BOOT owned (no voice, taps eaten), 2 s opens Settings first, swipe down/10 s/leave close, no scroll, comments PASS')
