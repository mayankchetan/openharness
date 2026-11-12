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
import os
import re
import subprocess
import tempfile

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


# BEHAVIOUR of the glue: the production menu helpers and ui_menu_boot, cut out of the source and run against the real menu_prompt.c.
harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "menu_prompt.h"
#define ESP_LOGI(tag, ...) ((void)0)
enum { HOME, SETTINGS };
static struct { ht_menu_t menu; int view, offset; } s;
static atomic_bool boot_level, menu_live;
static uint32_t clock_ms;
static int changes, locks, view_calls;
static bool asleep;
static uint32_t ms(void) { return clock_ms; }
static void change(void) { changes++; }
static void display_lock(void) { locks++; }
static void display_unlock(void) { locks--; }
static bool display_is_asleep(void) { return asleep; }
static void view(int v) { s.view = v; view_calls++; }
'''
for name in ('menu_sync', 'menu_open', 'menu_close', 'menu_tick', 'ui_menu_boot'):
    harness += function(name, raw) + chr(10)
harness += r'''
static void step(uint32_t t, bool down) { clock_ms = t; ui_menu_boot(down); menu_tick(t); }
int main(void)
{
    // THE TIMEOUT REPAINTS. The machine closes the prompt on its own; the glue must sync the flag and mark the scene dirty, or the prompt stays on the glass
    // while boot_voice (which keys on s.menu.open) stops refusing, and BOOT records under a visible prompt.
    clock_ms = 1000;
    menu_open();
    assert(s.menu.open && atomic_load(&menu_live) && changes == 1);
    int before = changes;
    menu_tick(10999);
    assert(s.menu.open && changes == before && "not yet");
    menu_tick(11000);
    assert(!s.menu.open && !atomic_load(&menu_live) && changes == before + 1 && "the timeout syncs and repaints");
    before = changes;
    menu_tick(12000);
    assert(changes == before && "nothing to do once it is gone");

    // Every other way out repaints too: a swipe down (menu_close), and a screen that went to sleep.
    clock_ms = 20000; menu_open(); before = changes;
    menu_close();
    assert(!s.menu.open && !atomic_load(&menu_live) && changes == before + 1);
    menu_close();
    assert(changes == before + 1 && "closing what is not up repaints nothing");
    clock_ms = 21000; menu_open(); before = changes; asleep = true;
    menu_tick(21500);
    assert(!s.menu.open && changes == before + 1);
    asleep = false;

    // A CLOSE WITH BOOT DOWN keeps the glue live until the release is eaten (menu_live), for the timeout and the swipe alike.
    atomic_store(&boot_level, true);
    clock_ms = 30000; menu_open();                      // BOOT already down: never counted
    menu_tick(40000);
    assert(!s.menu.open && atomic_load(&menu_live) && ht_menu_eats_tap(&s.menu));
    ui_menu_boot(false);
    assert(!atomic_load(&menu_live) && !ht_menu_eats_tap(&s.menu));
    atomic_store(&boot_level, false);

    // NO REPAINT AT 10 Hz FOR AN IDLE PROMPT: only while the bar moves (BOOT counted and down), and once more on the release that empties it.
    clock_ms = 50000; menu_open(); changes = 0;
    for (uint32_t t = 50100; t <= 53000; t += 100) step(t, false);
    assert(changes == 0 && s.menu.open && "BOOT up: the prompt is not repainted");
    for (uint32_t t = 53100; t <= 54000; t += 100) step(t, true);     // press edge at 53100, then held
    assert(changes >= 9 && changes <= 10 && "held: the bar moves every poll");
    changes = 0;
    step(54100, false);
    assert(changes == 1 && s.menu.open && "the release empties the bar: one repaint");
    step(54200, false); step(54300, false);
    assert(changes == 0 + 1 && "and then nothing");

    // THE CONFIRM opens the controls list at its first row, once, and the glue stays live until the release has been eaten.
    clock_ms = 60000; menu_open(); view_calls = 0;
    step(60100, false);
    for (uint32_t t = 60200; t <= 62100; t += 100) step(t, true);
    assert(view_calls == 0 && s.menu.open);
    step(62200, true);
    assert(view_calls == 1 && s.view == SETTINGS && s.offset == 0 && !s.menu.open && atomic_load(&menu_live));
    step(62300, true);
    assert(view_calls == 1 && atomic_load(&menu_live));
    step(62400, false);
    assert(!atomic_load(&menu_live) && locks == 0);
    puts("menu glue: timeout repaints, every close syncs, close with BOOT down, no 10 Hz repaint, confirm once PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-menu-') as directory:
    out = Path(directory)
    (out / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), '-I', str(habitat), str(out / 'test.c'),
                    str(habitat / 'menu_prompt.c'), '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)

# the pull up
assert '#define MENU_EDGE_Y 400' in ui
touch = function('habitat_touch')
assert re.search(r'else if \(surface && s\.menu\.open\) \{\s*if \(dy > 0\) menu_close\(\);\s*\} else if \(surface && s\.start_y >= MENU_EDGE_Y && dy < 0\) \{\s*menu_open\(\);\s*\} else if \(surface && character\.id == HT_CHARACTER_FOCUS && dy > 0 && notice_unread\(face_id\(\)\)\)', touch), \
    'dismiss and open come before the Inbox pull-down, which stays as it was'
ladder = touch[touch.index('gesture.axis == 1 && abs(dy) > 55 && abs(dy) > abs(dx)'):]
assert ladder.index('menu_open()') < ladder.index('view(INBOX)'), 'the pull up is judged inside the existing swipe threshold (55 px, vertical)'
assert 'if (ui_scroll_reportable()) {' in touch and 'MENU_EDGE_Y)) {' not in touch.split('ui_scroll_reportable()')[1][:80], 'the scroll begins as before (the fling brake needs it)'
# ... and is cancelled, without a fling, only when the pull is the prompt's, before ht_scroll_end would send the fling
assert re.search(r'const bool menu_pull = surface && !s\.menu\.open && !s\.touch_cancelled && s\.start_y >= MENU_EDGE_Y && gesture\.axis == 1 && pull_dy < 0 &&\s*abs\(pull_dy\) > 55 && abs\(pull_dy\) > abs\(pull_dx\) && !home_footer\(pressed_action\.kind\);\s*if \(menu_pull\) ht_scroll_cancel\(&scroll\);\s*bool scrolled = ht_scroll_end\(&scroll, x, y, now\);', touch), 'cancel, then end'
# a horizontal swipe is swallowed while the prompt is up
assert re.search(r'gesture\.axis == 2 && abs\(dx\) > 60 && abs\(dx\) > abs\(dy\)\) \{\s*if \(surface && s\.menu\.open\) \{[^}]*\} else if \(s\.view == TABS\) \{', touch), 'sideways swipes do not change the agent behind the prompt'
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
assert 'menu_tick(now);' in tick, 'the 10 s, and a sleeping screen'
mt = function('menu_tick')
assert re.search(r'if \(ht_menu_tick\(&s\.menu, now, atomic_load\(&boot_level\)\)\) \{[^}]*menu_sync\(\);\s*change\(\);\s*\}', mt), 'the timeout syncs and repaints'
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
