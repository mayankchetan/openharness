"""W16: the Link screen (USB or Wi-Fi, and Keep Wi-Fi), pinned without a toolchain for the dial.

The screen has two controls that matter more than their size: the Link row, and the Keep Wi-Fi toggle (stay_wl). A dial in Wi-Fi with stay_wl on never
looks at the cable, so the toggle is the ONLY way back, and it has to be reachable with no daemon at all. The rules, each checked on the source:

  1. The edge swipe (link_swipe) is a pure function, compiled and run here: bottom band, 55 px up, more up than sideways; never from above the band,
     never down, never a sideways stroke, never a short one. It opens the screen from the brand screen (no daemon needed) and from the home face
     (HOME and AGENT), ahead of the footer and face branches, and a stroke that starts in the band is not reported as a terminal scroll.
  2. The screen opens with no cable session: A_LINK is dispatched from the touch handler on `surface` only, the view's only way out is the same
     stroke (the generic bottom-edge swipe), the back arrow and a sideways swipe.
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

# ── 1. the stroke ───────────────────────────────────────────────────────────────────────────────────
edge_y = int(re.search(r'#define LINK_EDGE_Y (\d+)', raw)[1])
swipe_px = int(re.search(r'#define LINK_SWIPE_PX (\d+)', raw)[1])
assert edge_y == 400, 'the band is the one the generic close stroke uses (start_y >= 400)'
assert swipe_px >= 40, 'a thumb that slips is not a stroke'
assert re.search(r'else if \(s\.start_y >= 400 && dy < 0\)\s*view\(HOME\)', ui), 'the stroke that closes every other screen starts at y >= 400'

fn = re.search(r'static bool link_swipe\(int start_y, int dx, int dy\)\n\{.*?\n\}', raw, re.S)
assert fn, 'link_swipe'
driver = f'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#define LINK_EDGE_Y {edge_y}
#define LINK_SWIPE_PX {swipe_px}
{fn[0]}
int main(void)
{{
    assert(link_swipe(LINK_EDGE_Y, 0, -LINK_SWIPE_PX - 1));                  /* the lowest edge of the band, straight up */
    assert(link_swipe(450, 30, -120));                                       /* from the rim, a little sideways */
    assert(link_swipe(LINK_EDGE_Y, 59, -60));                                /* more up than sideways */
    assert(!link_swipe(LINK_EDGE_Y - 1, 0, -200));                           /* one pixel above the band: a face drag, a scroll */
    assert(!link_swipe(300, 0, -200));
    assert(!link_swipe(LINK_EDGE_Y, 0, -LINK_SWIPE_PX + 1));                 /* too short */
    assert(!link_swipe(LINK_EDGE_Y, 0, 0));                                  /* a tap */
    assert(!link_swipe(LINK_EDGE_Y, 0, 200));                                /* downwards */
    assert(!link_swipe(LINK_EDGE_Y, 90, -80));                               /* more sideways than up: the next agent */
    assert(!link_swipe(LINK_EDGE_Y, -80, -80));                              /* a tie is not "more up" */
    assert(!link_swipe(LINK_EDGE_Y, -200, -70));
    assert(!link_swipe(466, 0, -LINK_SWIPE_PX + 1));
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    src_path = Path(tmp) / 'link_swipe.c'
    src_path.write_text(driver)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=undefined,bounds', '-o', str(Path(tmp) / 'ls'), str(src_path)], check=True)
    subprocess.run([str(Path(tmp) / 'ls')], check=True)

touch = body(ui, 'habitat_touch')
# opened from the surface (HOME or AGENT), ahead of the footer and face branches
branch = re.search(r'else if \(surface && link_swipe\(s\.start_y, dx, dy\)\) \{(.*?)\n        \} else if', touch, re.S)
assert branch, 'the edge swipe branch'
assert 'dispatch((action_t){.kind = A_LINK})' in branch[1]
i_link = touch.index('link_swipe(s.start_y')
for later in ('pressed_action.kind==A_TABS', 'home_footer(pressed_action.kind))', 'pressed_action.kind == A_READER', 'pressed_action.kind == A_PET', 'HT_TOUCH_HOLD'):
    assert i_link < touch.index(later, touch.index('result == HT_TOUCH_TAP && s.touch_brake')), later
assert touch.count('link_swipe(') == 1 and re.findall(r'.kind = A_LINK}', ui) == ['.kind = A_LINK}'], 'one place opens the screen'
# a stroke that starts in the band is never a terminal scroll
assert re.search(r'if \(surface && \(home_footer\(pressed_action\.kind\) \|\| y >= LINK_EDGE_Y\)\)', touch), 'no scroll from the band'

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
assert 'config_net_set_mode(info.mode == 1 ? 0 : 1)' in work and 'info.saved && kind == A_LINK_MODE' in work

# ── 5. what the screen says ─────────────────────────────────────────────────────────────────────────
labels = re.findall(r'"((?:Link|Keep Wi-Fi)[^"]*)"', rawrender)
assert sorted(set(labels)) == ['Keep Wi-Fi: off', 'Keep Wi-Fi: on', 'Link: USB only', 'Link: Wi-Fi'], labels
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

print('link screen: edge swipe table, the stroke ahead of every other branch, no scroll from the band, no session needed, NVS on the worker only, '
      'stay_wl one toggling writer behind a whole configuration, labels, no credential, no cable path PASS')
