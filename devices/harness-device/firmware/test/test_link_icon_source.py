"""Wifi-first work, T5: source rules for the link icon, the Link screen's auto row and the dial's wording, checked without a toolchain.

  1. link_icon.c / link_icon.h are pure scene building (the battery rim's rule): no IDF, no clock, no allocation.
  2. ui_habitat.c adds the icon LAST (after the battery rim), marks the scene dirty from habitat_tick when kind or state changes, reads only the link state
     (net_link_state, ble_link_state) and the session, and gives the icon no hit area.
  3. The Link screen treats mode 3 (auto) like the phone mode: shown, never changed from the dial.
  4. Voice starts with the BOOT button only (commit 10ccf48fc): no text on the dial says "tap to talk" (or any tap that starts voice).
  5. The build and the test run know the new files.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'
habitat = main / 'ui/habitat'


def text(path):
    return path.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def code(path):
    src = re.sub(r'/\*.*?\*/', ' ', text(path), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


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


# 1. pure
for name in ('link_icon.c', 'link_icon.h'):
    src = code(habitat / name)
    includes = re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', src)
    assert set(includes) <= {'link_icon.h', 'terminal.h'}, f'{name} includes {sorted(set(includes))}'
    for banned in ('esp_', 'freertos', 'malloc', 'calloc', 'ESP_LOG', 'nvs_', 'config_', 'net_link', 'ble_link', 'cable_'):
        assert banned not in src, f'{name} mentions {banned}'
icon_c = code(habitat / 'link_icon.c')
assert 'if (s->count + HT_LINK_ICON_RUNS > HT_RUNS) return false;' in icon_c, 'the icon never displaces a screen\'s own content'
assert 'HT_LINK_ICON_RUNS = 4' in code(habitat / 'link_icon.h')

# 2. the wiring
ui = code(habitat / 'ui_habitat.c')
take = body(ui, 'habitat_scene_take')
assert take.index('render_battery_rim(f);') < take.index('render_link_icon(f);'), 'the icon is last, after the rim'
assert take.rstrip().endswith('return true;\n}') and take.index('render_link_icon(f);') > take.index('if (s.mode_notice) render_mode_notice(f);')
tick = body(ui, 'habitat_tick')
assert 'link_icon_state()' in tick and 's.link_icon_seen' in tick and 'change()' in tick, 'a change of kind or state marks the scene dirty'
kind = body(ui, 'link_icon_kind')
assert 'net_link_state() != NET_OFF' in kind and 'ble_link_state() != BLE_ST_OFF' in kind
render_icon = body(ui, 'render_link_icon')
assert 'ht_link_icon(f, link_icon_kind(), s.connected, &ink)' in render_icon and 'HT_THEME_WARN' in render_icon
assert 'hits' not in render_icon and 'control(' not in render_icon, 'a mark, not a control'
assert set(re.findall(r'\bnet_link_\w+', ui)) == {'net_link_status_text', 'net_link_state'}, 'the UI reads the caption and the state, nothing else of the link'

# 3. the Link screen
render_link = body(ui, 'render_link')
assert 'net_state_link_label(link_ui.wifi, link_ui.phone, link_ui.auto_on, ble_link_state() != BLE_ST_OFF)' in render_link
assert '!(mode && (link_ui.phone || link_ui.auto_on))' in render_link, 'the mode row does nothing in auto, as in the phone mode'
link_work = body(ui, 'link_work')
assert 'info.mode == 2 || info.mode == 3' in link_work and link_work.index('info.mode == 2 || info.mode == 3') < link_work.index('config_net_set_mode(next)'), 'auto is never turned into Wi-Fi from here'
assert 'link_ui.auto_on = info.saved && info.mode == 3;' in link_work
brand = body(ui, 'render_brand')
assert 'net_state_phone_caption()' in brand and 'ble_link_state() != BLE_ST_OFF' in brand and '!s.connected' in brand

# 4. wording: the only way to start voice on the dial is the BOOT button, so no string of the firmware's UI or link code says "tap to talk"
TAP = re.compile(r'tap\s+(?:the\s+\w+\s+)?to\s+(?:talk|speak|record|dictate)|tap\s+and\s+(?:talk|speak)|touch\s+to\s+(?:talk|speak)', re.I)
scanned = 0
for path in sorted(list(main.rglob('*.c')) + list(main.rglob('*.h')) + list(main.rglob('*.inc'))):
    for literal in re.findall(r'"(?:[^"\\\n]|\\.)*"', text(path)):
        scanned += 1
        assert not TAP.search(literal), f'{path.relative_to(main)}: {literal}'
assert scanned > 1000

# 5. build and run
assert '"ui/habitat/link_icon.c"' in text(main / 'CMakeLists.txt')
run = text(fw / 'test/run.sh')
for name in ('test_link_icon.c', 'test_link_icon_source.py', 'link_icon.c'):
    assert name in run, name
print('link icon source rules: pure builder, last in the scene, dirty mark from the tick, auto row read-only, no "tap to talk" in', scanned, 'strings PASS')
