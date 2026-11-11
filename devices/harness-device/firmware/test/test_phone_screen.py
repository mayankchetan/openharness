"""B5: the Phone screens (Settings > Phone, the number, Forget), pinned without a toolchain for the dial.

The screens are three views of ui_habitat.c (PHONE, PASSKEY, FORGET) over one struct, phone_ui. Rules, each checked on the source (the text helpers are compiled and run):

  1. THE ROW. "Phone" is a Settings row only in an image built with BLE (#ifdef DEVICE_BLE_LINK around the table entry), and it is enabled whatever the Wi-Fi
     or the daemon says (it sits in the always-enabled disjunct next to Inbox and Nap). The scroll limit of the list is the list's own count, not a number.
  2. THE STATE LINE: Not paired / Paired / Connected, Waiting for phone with a countdown while the window is open, Phone mode is off when the link is not running.
  3. PAIR opens the window (ble_link_pairing_window(true)) from the PHONE view only, and only when it is closed; leaving the three views closes it, and an unanswered
     number is a No (phone_leave, called from view()).
  4. THE NUMBER: PASSKEY shows six digits in Inter 44 with [yes] and [no]; Yes and No are the only callers of ble_link_pair_answer besides phone_leave;
     the window ending or the link going closes the view (phone_poll); the number is never logged.
  5. FORGET asks once ("forget phone?", [no] [yes]) and Yes only POSTS the request: ble_link_forget is called in exactly one place, the Yes case; the worker, the
     Link worker and the renderer never reach it, and nothing in the UI names the bond store. The wipe itself is on the NimBLE host task (test_ble_link_source.py).
  6. THE RENDERERS draw phone_ui alone (no call into ble_link), so the gallery can set it; the poll is not compiled into the gallery image.
  7. THE GLASS: every control and line sits inside the safe circle (r 230 about 233,233), as the other screens do.
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

# ── 1. the row ──────────────────────────────────────────────────────────────────────────────────────
items = body(raw, 'settings_item')
assert re.search(r'#ifdef DEVICE_BLE_LINK\n\s*\{"Phone", A_PHONE, 0\},[^\n]*\n#endif', items), 'the Phone row is inside #ifdef DEVICE_BLE_LINK'
assert items.count('DEVICE_BLE_LINK') == 1 and 'feature' in items
assert '"Phone", A_PHONE, 0}' in items and 'CABLE_FEATURE' not in items[items.index('"Phone"'):items.index('"Phone"') + 30], 'no daemon feature gates the row'
settings = body(ui, 'render_settings')
assert re.search(r'bool enabled = action == A_INBOX \|\| action == A_NAP \|\| action == A_PHONE \|\|\s*\(s\.connected', settings), 'always enabled, like Inbox and Nap'
assert 'wifi' not in items.lower() and 'config_net' not in items
assert re.search(r'view_t;', raw) and all(re.search(r'\b' + v + r'\b', raw[raw.index('typedef enum {\n    HOME'):raw.index('} view_t;')]) for v in ('PHONE', 'PASSKEY', 'FORGET'))
assert 'case PHONE: return "phone";' in raw and 'case PASSKEY: return "passkey";' in raw and 'case FORGET: return "forget";' in raw
assert 'case A_PHONE:' in ui and re.search(r'case A_PHONE:\s*memset\(&phone_ui, 0, sizeof phone_ui\);\s*view\(PHONE\);', ui)
assert 'settings_count()' in ui[ui.index(': s.view == SETTINGS ?'):ui.index(': s.view == SETTINGS ?') + 60], 'the swipe limit follows the list'

# ── 2. the state line ──────────────────────────────────────────────────────────────────────────────
phone = body(raw, 'render_phone')
for label in ('"Not paired"', '"Paired"', '"Connected"', '"Waiting for phone"', '"Phone mode is off"', '"[pair]"', '"[forget]"'):
    assert label in phone, label
assert phone.index('"Waiting for phone"') < phone.index('"Connected"') < phone.index('"Paired"') < phone.index('"Not paired"'), 'the window beats a link, a link beats a bond'
assert 'phone_left_text(phone_ui.left_s' in phone
assert re.search(r'control\(f, \d+, \d+, \d+, "\[forget\]", A_PHONE_FORGET_ASK, 0, idle && phone_ui\.bonds > 0\)', phone), 'Forget needs a closed window and a bond'
assert re.search(r'control\(f, \d+, \d+, \d+, "\[pair\]", A_PHONE_PAIR, 0, idle\)', phone) and 'const bool idle = phone_ui.running && !phone_ui.window;' in phone

# ── 3. Pair, back ───────────────────────────────────────────────────────────────────────────────────
disp = body(ui, 'dispatch')
pair = re.search(r'case A_PHONE_PAIR:(.*?)break;', disp, re.S)[1]
assert 's.view == PHONE' in pair and '!phone_ui.window' in pair and 'phone_ui.running' in pair and 'ble_link_pairing_window(true)' in pair
assert 'ble_link_state() == BLE_ST_NOT_FOUND' in pair and 'ble_link_restart()' in pair, '"Phone not found" is left at once'
assert ui.count('ble_link_pairing_window(true)') == 1
leave = body(raw, 'phone_leave')
assert 'if (s.view == PASSKEY) ble_link_pair_answer(false);' in leave and 'ble_link_pairing_window(false)' in leave and 'memset(&phone_ui, 0, sizeof phone_ui)' in leave
assert leave.index('ble_link_pair_answer(false)') < leave.index('ble_link_pairing_window(false)') < leave.index('memset'), 'No first (it needs the view), then the window, then forget the number'
view_fn = body(raw, 'view')
# Item 4b (2026-11-10): only a TOUCH ends the window. user_nav is true exactly while habitat_touch runs (a cleanup-scoped flag, so every return clears it); a boot
# landing (ui_land_after_reload), a refresh or a desktop message that moves the view leaves the window and the offer alone, and phone_poll brings the screen back.
assert re.search(r'if \(phone_view\(s\.view\) && !phone_view\(v\)\) \{\s*if \(user_nav\) phone_leave\(\);[^\n]*\n\s*else ESP_LOGI\("habitat", "phone: the view moved off the Phone screen without a touch;[^\n]*\n\s*\}\s*s\.view = v;', view_fn), 'only a touch closes the window'
assert 'phone_leave()' not in view_fn.replace('if (user_nav) phone_leave();', ''), 'view() has no other path to phone_leave'
assert len(re.findall(r'\bphone_leave\(', code('ui/habitat/ui_habitat.c'))) == 3, 'declaration, definition and the one call in view()'
touch_fn = body(raw, 'habitat_touch')
assert re.search(r'bool user_nav_scope __attribute__\(\(cleanup\(user_nav_end\)\)\) = user_nav = true;', touch_fn), 'the flag is set in habitat_touch and cleared by the cleanup at every return'
assert 'static void user_nav_end(bool *scope) { (void)scope; user_nav = false; }' in raw and 'static bool user_nav;' in raw
assert len(re.findall(r'\buser_nav = true\b', code('ui/habitat/ui_habitat.c'))) == 1, 'nothing but habitat_touch sets it'
land = body(raw, 'ui_land_after_reload')
assert 'phone_leave' not in land and 'user_nav' not in land, 'the boot landing is not the user'
assert 'phone: the user left the Phone screen' in body(raw, 'phone_leave'), 'a close by the user says so in the log (item 4c)'
# Review M-1(c): the screen is woken when the window opens and when a number appears, once each, never at every tick
poll_b = body(raw, 'phone_poll')
assert re.search(r'if \(!window\) bumped_window = false;\s*if \(!prompt\) bumped_prompt = false;\s*if \(window && !bumped_window\) \{ bumped_window = true; display_bump_activity\(\); \}\s*if \(prompt && !bumped_prompt\) \{ bumped_prompt = true; display_bump_activity\(\); \}', poll_b), 'one bump per window open and per number'
# Review F-1 (2026-11-11): the marks live at file scope (not in phone_poll, which returns early off the Phone views), and leaving the Phone screen clears them, so a
# window that a later connect re-opens while the user is elsewhere is bumped again. A static inside phone_poll could not be reset from phone_leave.
assert raw.count('static bool bumped_window, bumped_prompt;') == 1 and raw.index('} phone_ui;') < raw.index('static bool bumped_window, bumped_prompt;') < raw.index('static void phone_poll(void)\n{'), 'file-scope marks, after phone_ui and before phone_poll'
assert 'bumped_window = false;' in leave and 'bumped_prompt = false;' in leave, 'phone_leave clears both marks'
assert leave.index('memset(&phone_ui, 0, sizeof phone_ui)') < leave.index('bumped_window = false;'), 'the marks are cleared with the view state, after the window closes'
assert 'if (window || prompt) display_bump_activity()' not in poll_b, 'no bump at every tick'
assert 'case FORGET:' in body(raw, 'ui_mode_switch_busy'), 'the question holds the mode switch like the stop question'

# ── 4. the number ───────────────────────────────────────────────────────────────────────────────────
answers = [m.start() for m in re.finditer(r'ble_link_pair_answer\(', ui)]
assert len(answers) == 2, 'two callers: Yes/No, and phone_leave'
yes_no = re.search(r'case A_PHONE_YES:\s*case A_PHONE_NO:(.*?)break;', disp, re.S)[1]
assert 's.view == PASSKEY' in yes_no and 'ble_link_pair_answer(a.kind == A_PHONE_YES)' in yes_no
passkey = body(raw, 'render_passkey')
assert re.search(r'control\(f, \d+, \d+, \d+, "\[yes\]", A_PHONE_YES, 0, true\)', passkey) and re.search(r'control\(f, \d+, \d+, \d+, "\[no\]", A_PHONE_NO, 0, true\)', passkey)
assert '&ht_lv_inter_44.base' in passkey and 'ht_pixel_40' not in raw and 'phone_code_text(phone_ui.code' in passkey
poll = body(raw, 'phone_poll')
assert 'if (prompt && s.view == PHONE) view(PASSKEY);' in poll and 'else if (!prompt && s.view == PASSKEY) view(PHONE);' in poll
assert 'ble_link_pair_prompt(&code)' in poll and 'phone_ui.code = prompt ? code % 1000000 : 0;' in poll
assert re.search(r'bonds > phone_ui\.bonds', poll) and 'ble_link_pairing_window(false)' in poll, 'a new bond ends the window'
# AUTO-OPEN (2026-11-09): no touch path reaches Settings, so when the link has opened the window by itself (no bond stored) the poll shows the Phone screen. It
# does so only while the window (or a number) is up and the view is not already one of the three; leaving the three views closes the window (rule 3), so it cannot
# pull the user back; [pair] stays the only UI caller that opens a window.
assert re.search(r'if \(!phone_view\(s\.view\)\) \{\s*if \(!ble_link_pairing_busy\(\)\) return;\s*memset\(&phone_ui, 0, sizeof phone_ui\);\s*view\(PHONE\);[^\n]*\n\s*if \(!phone_view\(s\.view\)\) return;\s*\}', poll), 'outside the views: only a busy link (window or number) shows the Phone screen, and a refused move is left alone'
assert poll.index('ble_link_pairing_busy()') < poll.index('view(PHONE)') < poll.index('ble_link_state()'), 'the move comes before the read'
assert ui.count('ble_link_pairing_window(true)') == 1, 'the UI opens no window of its own: the link does it, and [pair] is the one call'
for m in re.finditer(r'(?:ESP_LOG[EWIDV]|printf)\((.*?)\);', raw, re.S):
    assert 'phone_ui' not in m[1], 'the number is never logged'

# ── 5. Forget ───────────────────────────────────────────────────────────────────────────────────────
forget = body(raw, 'render_forget')
assert '"forget phone?"' in forget and re.search(r'control\(f, \d+, \d+, \d+, "\[no\]", A_PHONE, 0, true\)', forget) and re.search(r'control\(f, \d+, \d+, \d+, "\[yes\]", A_PHONE_FORGET_YES, 0, true\)', forget)
assert re.search(r'case A_PHONE_FORGET_ASK:\s*if \(s\.view == PHONE && phone_ui\.running && !phone_ui\.window && phone_ui\.bonds\) view\(FORGET\);', disp)
fy = re.search(r'case A_PHONE_FORGET_YES:(.*?)break;', disp, re.S)[1]
assert 's.view == FORGET' in fy and fy.count('ble_link_forget()') == 1
assert ui.count('ble_link_forget(') == 1, 'one caller in the whole UI file: the Yes case'
assert 'ble_store' not in ui, 'the UI never names the bond store'
for fn in ('worker', 'link_work', 'render_phone', 'render_passkey', 'render_forget', 'phone_poll', 'habitat_tick'):
    assert 'ble_link_forget' not in body(ui, fn), f'{fn} must not reach Forget'
assert 'A_PHONE_FORGET_YES' not in body(ui, 'worker'), 'Forget is not a worker action: the host task does the wipe'
# nowhere else in the firmware: the UI file is the only caller outside ble_link.c itself
callers = [p.name for p in main.rglob('*.c') if p.name not in ('ble_link.c',) and re.search(r'\bble_link_forget\(', code(str(p.relative_to(main)).replace('\\', '/')))]
assert callers == ['ui_habitat.c'], callers
assert [p.name for p in main.rglob('*.c') if re.search(r'\bble_store_clear\(', code(str(p.relative_to(main)).replace('\\', '/')))] == ['ble_link.c']

# ── 6. the renderers, and the gallery ────────────────────────────────────────────────────────────────
for fn in ('render_phone', 'render_passkey', 'render_forget'):
    assert 'ble_link_' not in body(ui, fn), f'{fn} draws phone_ui only'
tick = body(raw, 'habitat_tick')
assert re.search(r'#ifndef DEVICE_UI_GALLERY\n\s*phone_poll\(\);[^\n]*\n#endif', tick)
assert re.search(r'if \(phone_view\(s\.view\) && delay > 250\) delay = 250;', raw)

# ── 7. the glass ────────────────────────────────────────────────────────────────────────────────────
MONO28 = (17, 38)


def inside(x0, y0, x1, y1):
    return all((x - 233) ** 2 + (y - 233) ** 2 <= 230 ** 2 for x in (x0, x1) for y in (y0, y1))


def fn_body(name):
    b = body(raw, name)
    return b


for fn in ('render_phone', 'render_passkey', 'render_forget'):
    b = fn_body(fn)
    for x, y, w, label in re.findall(r'control\(f, (\d+), (\d+), (\d+), ("[^"]*")', b):
        x, y, w = int(x), int(y), int(w)
        assert inside(x, y - 14, x + w, y + 52), f'{fn} {label}: the hit rect leaves the circle'
        assert (len(label) - 2) * MONO28[0] <= w, f'{fn} {label}: wider than its rect'
    for y, label in re.findall(r'center\(f, (\d+), ("[^"]*")', b):
        y, n = int(y), len(label) - 2
        assert inside(233 - n * MONO28[0] // 2, y, 233 + n * MONO28[0] // 2, y + MONO28[1]), f'{fn} {label}: leaves the circle'
    for x, y, w, rows in re.findall(r'ui_wrap\(f, FACE_CX\((\d+)\), (\d+), (\d+), (\d+),', b):
        assert inside(233 - int(w) // 2, int(y), 233 + int(w) // 2, int(y) + 28 * int(rows)), f'{fn}: a wrapped block leaves the circle'
assert inside(233 - 140, 150, 233 + 140, 150 + 59), 'the number: seven glyphs of Inter 44 are under 280 px (digits are about 27 px), 59 px tall'
assert re.search(r'ht_text\(f, \(HT_WIDTH - w\) / 2, 150, w, big, FG, BG, code\)', passkey) and 'ht_measure(big, code)' in passkey
# Review M2 (2026-11-10): the auto-opened window can show a number the user did not start; the screen says when it is theirs. Two rows of the 12 x 28 mono face, between the question and the buttons.
assert 'ui_wrap(f, FACE_CX(336), 256, 336, 2, 0, &ht_mono_20, DIM, "Press No unless your phone shows this number");' in passkey
assert len("Press No unless your phone shows this number") <= 2 * (336 // 12), 'two rows of 28 cells'
assert 256 + 2 * 28 <= 340 - 14 + 2, 'the text ends where the hit rects of [yes] and [no] begin'
assert 214 + 38 <= 256 and 150 + 59 <= 214, 'no overlap with the question or the number'

# ── the text helpers, compiled ──────────────────────────────────────────────────────────────────────
helpers = body(raw, 'phone_code_text'), body(raw, 'phone_left_text')
driver = f'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void phone_code_text(uint32_t code, char *out, size_t cap)
{helpers[0]}
static void phone_left_text(uint32_t seconds, char *out, size_t cap)
{helpers[1]}
int main(void)
{{
    char b[16];
    phone_code_text(0, b, sizeof b);       assert(!strcmp(b, "000 000"));
    phone_code_text(7, b, sizeof b);       assert(!strcmp(b, "000 007"));
    phone_code_text(123456, b, sizeof b);  assert(!strcmp(b, "123 456"));
    phone_code_text(999999, b, sizeof b);  assert(!strcmp(b, "999 999"));
    phone_code_text(1234567, b, sizeof b); assert(!strcmp(b, "234 567"));   /* the poll keeps it under 1,000,000; this never overflows the box */
    assert(strlen(b) == 7);
    phone_left_text(120, b, sizeof b);     assert(!strcmp(b, "2:00 left"));
    phone_left_text(59, b, sizeof b);      assert(!strcmp(b, "0:59 left"));
    phone_left_text(1, b, sizeof b);       assert(!strcmp(b, "0:01 left"));
    phone_left_text(0, b, sizeof b);       assert(!strcmp(b, "0:00 left"));
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as d:
    src = Path(d) / 'phone_text.c'
    src.write_text(driver)
    exe = Path(d) / 'phone_text'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)

print('phone screen: BLE-only Settings row, state line, Pair/back window rule, number Yes/No, Forget posted once, glass bounds PASS')
