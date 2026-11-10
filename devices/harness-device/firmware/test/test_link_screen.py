"""W16: the Link screen (USB or Wi-Fi, and Keep Wi-Fi), pinned without a toolchain for the dial.

The screen has two controls that matter more than their size: the Link row, and the Keep Wi-Fi toggle (stay_wl). A dial in Wi-Fi with stay_wl on never
looks at the cable, so the toggle is the ONLY way back, and it has to be reachable with no daemon at all. The rules, each checked on the source:

  1. NO WAY IN FROM THE DIAL (user decision 2026-11-10): the edge swipe is gone. link_swipe, LINK_EDGE_Y and LINK_SWIPE_PX do not exist, nothing dispatches A_LINK,
     and view(LINK) is called nowhere in the firmware but the visual-only UI gallery (DEVICE_UI_GALLERY). The way in on the computer is `harness dial wifi on|off`.
     The scroll of a drag that starts in the bottom band is reported again (that exclusion belonged to the swipe).
  2. The screen, kept for a later entry point: A_LINK still opens it with no cable session, the view's way out is the generic edge stroke, the back arrow and a
     sideways swipe. In the phone mode (wl_mode 2) the Link row says "Link: Phone", is not a hit target, and link_work never writes mode 1 from mode 2; every
     write from the screen logs the old and the new value.
  3. NVS is touched by the worker alone (link_work), never by the render task or by dispatch; the screen's copy is written under the display lock.
  4. stay_wl: config_save_stay_wl is called exactly once in the whole firmware outside config_store.c, in link_work, as a TOGGLE (it can write false
     as well as true), only behind a whole Wi-Fi configuration, and the mode switch is told afterwards (mode_glue_settings_changed).
  5. The screen shows no network name, password or key; its labels are the ones the user note names.
  6. No path to the controls from the cable: nothing in cable_client.c, debug_cmd or the daemon-facing code calls the setters or queues the actions.
"""
from pathlib import Path
import re
import subprocess
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
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


UI = 'ui/habitat/ui_habitat.c'
raw = text(UI)
ui = code(UI)

# ── 1. no way in from the dial ───────────────────────────────────────────────────────────────────────────────────
for gone in ('link_swipe', 'LINK_EDGE_Y', 'LINK_SWIPE_PX'):
    assert gone not in raw, f'{gone} is back: the edge swipe to the Link screen was removed (user decision 2026-11-10)'
touch = body(ui, 'habitat_touch')
assert 'A_LINK' not in touch.replace('A_LINK_', ''), 'the touch handler opens no Link screen'
assert not re.search(r'\bA_LINK\b', re.sub(r'case A_LINK:', '', re.sub(r'^\s*A_LINK,', '', ui, flags=re.M))), 'nothing dispatches A_LINK (the enum value and its case remain)'
assert not re.search(r'\.kind\s*=\s*A_LINK\b(?!_)', ui) and not re.search(r'queue\(\(action_t\)\{\.kind\s*=\s*A_LINK\b(?!_)', ui)
# view(LINK) is called nowhere on the dial: ui_habitat.c has none, and the one other place is the gallery build
assert 'view(LINK)' in code('ui/habitat/ui_habitat.c'), 'dispatch(A_LINK) still draws the screen for a later entry point'
callers = [str(q.relative_to(main)).replace('\\', '/') for q in main.rglob('*') if q.suffix in ('.c', '.h', '.inc') and 'view(LINK)' in code(str(q.relative_to(main)).replace('\\', '/'))]
assert sorted(callers) == ['ui/habitat/ui_gallery.inc', 'ui/habitat/ui_habitat.c'], callers
assert re.search(r'case A_LINK:\s*//[^\n]*\n[^\n]*\n\s*link_ui\.loaded = false;\s*view\(LINK\);', raw), 'the one view(LINK) in the UI is the A_LINK case'
assert '#ifdef DEVICE_UI_GALLERY\n#include "ui_gallery.inc"' in raw, 'the gallery draws the screen only in the visual-only build'
# the exclusion that belonged to the swipe is gone: a drag that starts in the bottom band is a terminal scroll again
assert re.search(r'if \(surface && home_footer\(pressed_action\.kind\)\) \{', touch), 'only the footer is excluded from the scroll'

# ── 2. reachable with no daemon ─────────────────────────────────────────────────────────────────────
dispatch = body(ui, 'dispatch')
case = re.search(r'case A_LINK:(.*?)case A_LINK_MODE:', dispatch, re.S)[1]
assert 'view(LINK)' in case and 'A_LINK_SYNC' in case
assert 's.connected' not in case and 'cable_client' not in case, 'opening the screen needs no session'
rest = re.search(r'case A_LINK_MODE:\s*case A_LINK_STAY:(.*?)break;', dispatch, re.S)[1]
assert 's.view == LINK' in rest and 'queue(a)' in rest and 's.connected' not in rest, 'the toggles need no session either'
assert 'config_' not in case and 'config_' not in rest, 'dispatch (the touch task) writes no flash'
assert 'case LINK:\n        render_link(f);' in raw
assert 'case LINK: return "link";' in raw
view_enum = re.search(r'typedef enum \{(.*?)\} view_t;', raw, re.S)[1]
assert 'LINK' in view_enum
# the way out: the back arrow of heading(), the generic edge stroke, and a sideways swipe (all existing code for every non-surface view)
rawrender = body(raw, 'render_link')
assert 'heading(f, "link")' in rawrender
assert re.search(r'\} else if \(!surface\)\s*view\(HOME\);', touch)

# ── 3. flash only on the worker ─────────────────────────────────────────────────────────────────────
render = body(ui, 'render_link')
for banned in ('config_', 'nvs_', 'cable_client_', 'esp_ota', 'esp_partition'):
    assert banned not in render, f'render_link calls {banned}'
work = body(ui, 'link_work')
worker = body(ui, 'worker')
assert re.search(r'case A_LINK_SYNC:\s*case A_LINK_MODE:\s*case A_LINK_STAY:\s*link_work\(a\.kind\);', worker)
for name in ('config_net_set_mode', 'config_save_stay_wl', 'config_net_info', 'config_load_stay_wl'):
    assert name in work, name
    others = [m.start() for m in re.finditer(r'\b' + name + r'\(', ui)]
    inside = [m.start() for m in re.finditer(r'\b' + name + r'\(', work)]
    if name in ('config_net_set_mode', 'config_save_stay_wl'):
        assert len(others) == len(inside), f'{name} is called only in link_work'
# the copy the render task reads is written under the display lock, after both writes
assert work.index('config_net_set_mode(') < work.index('display_lock()') and work.index('config_save_stay_wl(') < work.index('display_lock()')
assert work.index('display_lock()') < work.index('link_ui.loaded = true') < work.index('display_unlock()')
assert 'change()' in work

# W16b: the worker's stack is measured once, after the first write (6,144 B, PSRAM, three NVS commits, not measured before)
rawwork = body(raw, 'link_work')
assert re.search(r'static bool hwm_logged;\s*if \(kind != A_LINK_SYNC && !hwm_logged\) \{\s*hwm_logged = true;\s*ESP_LOGI\("habitat", "link_work stack free_min=%u B of 6144", \(unsigned\)uxTaskGetStackHighWaterMark\(NULL\)\);', rawwork)
# ── 4. stay_wl: one writer, a toggle, behind a whole configuration, and the switch is told ──────────
writers = []
for path in main.rglob('*.c'):
    rel = str(path.relative_to(main)).replace('\\', '/')
    if rel == 'config_store.c':
        continue
    writers += [rel] * len(re.findall(r'\bconfig_save_stay_wl\(', code(rel)))
assert writers == [UI], f'config_save_stay_wl callers: {writers}'
assert 'config_save_stay_wl(!config_load_stay_wl())' in work, 'a toggle: it writes false as well as true'
assert re.search(r'if \(info\.saved && kind == A_LINK_STAY\)', work), 'only behind a whole Wi-Fi configuration'
assert 'config_save_stay_wl(true)' not in ui and 'config_save_stay_wl(1)' not in ui
assert re.search(r'if \(kind != A_LINK_SYNC\) mode_glue_settings_changed\(\);', work)
assert '#include "mode_glue.h"' in raw
# USB while Wi-Fi runs: the same flip, and the glue (mode_switch.c H12) does the rest through the busy gate
assert 'info.saved && kind == A_LINK_MODE' in work and 'const int next = info.mode == 1 ? 0 : 1;' in work and 'config_net_set_mode(next)' in work
# the phone mode (wl_mode 2) is never turned into Wi-Fi from here, and every write says what it did (review of 2026-11-10)
assert re.search(r'if \(info\.mode == 2\) \{\s*ESP_LOGW\("habitat", "link: wl_mode 2 \(phone\) is not changed from the Link screen"\);\s*\} else \{', rawwork), 'mode 2 is left alone and says so'
assert rawwork.index('info.mode == 2') < rawwork.index('config_net_set_mode(next)'), 'the phone check comes before the write'
assert 'config_net_set_mode(info.mode == 1 ? 0 : 1)' not in rawwork, 'the old flip (2 -> 1) is gone'
assert re.search(r'ESP_LOGI\("habitat", "link: wl_mode %d -> %d from the Link screen: %s", \(int\)info\.mode, next, ok \? "ok" : "FAILED"\);', rawwork), 'every wl_mode write from the screen logs old, new and the result'
assert rawwork.count('config_net_set_mode(') == 1 and rawwork.index('config_net_set_mode(next)') < rawwork.index('link: wl_mode %d -> %d')
assert 'link_ui.phone = info.saved && info.mode == 2;' in rawwork
assert re.search(r'!\(mode && link_ui\.phone\)\};', rawrender), 'the Link row is not a hit target in the phone mode'

# ── 5. what the screen says ─────────────────────────────────────────────────────────────────────────
labels = re.findall(r'"((?:Link|Keep Wi-Fi)[^"]*)"', rawrender)
assert sorted(set(labels)) == ['Keep Wi-Fi: off', 'Keep Wi-Fi: on', 'Link: Phone', 'Link: USB only', 'Link: Wi-Fi'], labels
assert all(len(label) <= 17 for label in labels), 'a row of the mono skin is 17 cells'
assert 'net_state_line(net_link_state())' in rawrender
for banned in ('ssid', 'pass', 'psk', 'key', 'host'):
    assert not re.search(r'\b' + banned, render, re.I), f'render_link mentions {banned}'
assert 'config_net_load' not in ui, 'the screen never reads the secrets'

# ── 6. no way in from the cable ─────────────────────────────────────────────────────────────────────
for rel in ('cable_client.c', 'cable_debug.c', 'debug_cmd.c', 'debug_cmd_json.c', 'cable_link.c', 'app_main.c'):
    src = code(rel)
    assert 'A_LINK' not in src and 'config_save_stay_wl' not in src, rel
assert 'config_net_set_mode(' not in code('cable_debug.c') and 'config_net_set_mode(' not in code('debug_cmd.c')

# ── 7. the build and the run know the new test ──────────────────────────────────────────────────────
assert 'test_link_screen.py' in (fw / 'test/run.sh').read_text()

print('link screen: no edge swipe and nothing dispatches it, view(LINK) only in the gallery build, no scroll exclusion left, no session needed, NVS on the worker only, '
      'phone mode never turned into Wi-Fi and every write logged, stay_wl one toggling writer behind a whole configuration, labels, no credential, no cable path PASS')
