"""The controls list as three groups with a heading (dial-reach T10; plan amendment B2 and B3; user decisions 2026-11-12).

The list opens from the menu prompt (BOOT held 2 s). It reads MENU on top (as the Settings screens read SETTINGS), and its rows are in three groups
with a dim label line before each group's first visible row: THIS AGENT, ON COMPUTER, DIAL. The model is menu_list.c (test_menu_list.c runs its rules);
this test reads ui_habitat.c (comments removed) and compiles the production table and its helpers against that model.

  * the table: ten rows, in this order, with these groups and features, sorted by group;
  * the old words are gone from the table; the heading is MENU and no "controls" heading is left;
  * a label line registers no hit (the label branch has no hit_count++ and calls no control()), and it is a full list slot;
  * the scroll goes through menu_scroll, a drag that starts on a label is still the view's, the back from a Settings screen goes through menu_clamp;
  * BOOT held opens the menu scrolled to its END, so the DIAL group with Settings is on screen (user decision 2026-11-12; it replaced "opens at the top");
  * the production table, settings_item, settings_count and settings_end_offset compiled with a stub host-feature test give the 13-line sequence.
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


ui = code(habitat / 'ui_habitat.c')


def function(name, src=ui):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, 'no definition of ' + name
    return match.group(0)


# --- the table --------------------------------------------------------------------------------------------------------------------------------
table_match = re.search(r'static const struct \{[^}]*\} menu_items\[\] = \{(.*?)\n\};', ui, re.S)
assert table_match, 'menu_items[] is a file-scope table with a group field'
table = table_match.group(1)
rows = re.findall(r'\{"([^"]+)",\s*(A_\w+),\s*([A-Za-z_0-9]+),\s*(MENU_GROUP_\w+)\}', table)
WANT = [
    ('Stop turn', 'A_STOP', '0', 'MENU_GROUP_AGENT'),
    ('Change model', 'A_MODELS', '0', 'MENU_GROUP_AGENT'),
    ('Latest reply', 'A_LATEST', 'CABLE_FEATURE_VISIT', 'MENU_GROUP_AGENT'),
    ('Find agent', 'A_FIND', 'CABLE_FEATURE_FORM', 'MENU_GROUP_COMPUTER'),
    ('New agent', 'A_FORM', 'CABLE_FEATURE_FORM', 'MENU_GROUP_COMPUTER'),
    ('Pick text', 'A_SELECT_BEGIN', 'CABLE_FEATURE_SELECTION', 'MENU_GROUP_COMPUTER'),
    ('Inbox', 'A_INBOX', '0', 'MENU_GROUP_DIAL'),
    ('Quiet 15 min', 'A_NAP', '0', 'MENU_GROUP_DIAL'),
    ('Computers', 'A_MACHINES', '0', 'MENU_GROUP_DIAL'),
    ('Settings', 'A_PREFS', '0', 'MENU_GROUP_DIAL'),
]
# T7 adds {"All agents", A_RUNNING, CABLE_FEATURE_RUNNING, MENU_GROUP_COMPUTER} as the FIRST ON COMPUTER row (test_running_ui.py pins it);
# this test accepts either shape, and checks the order of everything else.
ALL_AGENTS = ('All agents', 'A_RUNNING', 'CABLE_FEATURE_RUNNING', 'MENU_GROUP_COMPUTER')
if ALL_AGENTS in rows:
    assert rows.index(ALL_AGENTS) == 3, 'All agents is the first row of ON COMPUTER'
    rows.remove(ALL_AGENTS)
assert rows == WANT, 'the table is the ten rows in this order: %r' % (rows,)
order = [r[3] for r in rows]
assert order == sorted(order, key=['MENU_GROUP_AGENT', 'MENU_GROUP_COMPUTER', 'MENU_GROUP_DIAL'].index), 'sorted by group'
assert re.search(r'_Static_assert\([^;]*MENU_GROUPS[^;]*<=\s*MENU_MAX_LINES', ui), 'items + groups fit MENU_MAX_LINES (compile-time)'

settings_functions = ''.join(function(n) for n in ('settings_lines', 'settings_item', 'settings_count', 'settings_end_offset'))
for old in ('Stop current turn', 'Machines', 'Latest output', 'Select text', 'Nap', 'Find Harness', 'New Harness', 'Model"'):
    assert '"' + old.rstrip('"') + '"' not in table + settings_functions, 'the old label %s is gone from the menu table' % old
assert '"Settings", A_PREFS, 0, MENU_GROUP_DIAL' in table.replace('\n', ' ') or ('Settings', 'A_PREFS', '0', 'MENU_GROUP_DIAL') in WANT, 'Settings is the last DIAL row'
assert 'menu_build(' in function('settings_lines') and 'cable_client_supports' in function('settings_lines')

# --- the screen ---------------------------------------------------------------------------------------------------------------------------------
render = function('render_settings')
assert 'focus_header(f, "MENU")' in render, 'the heading is MENU, drawn like the Settings screens'
assert re.search(r'control\(f, 87, 67, 48, "<", A_HOME, 0, true\)', render), 'the < control leaves to Home'
assert 'heading(f, "controls")' not in render and '"controls"' not in render, 'no lowercase controls heading'
assert re.search(r's\.offset\s*=\s*menu_clamp\(', render), 'the offset is clamped by the model on every draw'
branch = render[render.index('line.item < 0') if 'line.item < 0' in render else 0:]
label_branch = branch[:branch.index('continue;') + len('continue;')] if 'continue;' in branch else ''
assert label_branch and 'menu_label(' in label_branch, 'a label line is drawn by menu_label and skips the hit code'
assert 'hit_count' not in label_branch and 'control(' not in label_branch and 'hits[' not in label_branch, 'a label registers no hit'
label_fn = function('menu_label')
assert 'hit_count' not in label_fn and 'control(' not in label_fn and 'hits[' not in label_fn, 'menu_label registers no hit either'
assert render.count('s.hit_count++') == 1, 'one hit per ROW, none per label'
for words in ('"THIS AGENT"', '"ON COMPUTER"', '"DIAL"'):
    assert words in ui, words + ' is a label'
assert re.search(r'"THIS AGENT: %s"|"THIS AGENT: "', ui) or 'THIS AGENT: ' in label_fn, 'THIS AGENT carries the on-screen agent name'
assert 'ht_fit_width(' in label_fn and 'SET_TEXT_W' in label_fn, 'the name is fitted to the row width'
assert 'active()' in label_fn, 'the name is the on-screen agent'
assert 'ht_lv_inter_20' in label_fn and '0x4c4c4c' in label_fn and 'ht_mono_16' in label_fn and 'DIM' in label_fn, 'dim label text in both skins'
assert re.search(r'bool enabled = action == A_INBOX \|\| action == A_NAP \|\| action == A_PHONE \|\| action == A_PREFS \|\|\s*\(s\.connected', render), \
    'the enabled rule is as it was'

# --- scroll, back, BOOT -------------------------------------------------------------------------------------------------------------------------
move = function('tabs_move')
assert re.search(r'if \(s\.view == SETTINGS\) \{[^}]*menu_scroll\(', move), 'the menu scrolls through menu_scroll'
assert 's.view == AGENTS ? s.count' in move, 'the other lists keep their code'
sw = ui[ui.index('case A_SETTINGS: {'):]
sw = sw[:sw.index('case A_PREFS')]
assert 'menu_clamp(' in sw and 's.list_offset' in sw, 'back from a Settings screen: the kept offset goes through menu_clamp'
boot = function('ui_menu_boot')
assert re.search(r'view\(SETTINGS\);\s*s\.offset = settings_end_offset\(\);', boot), 'BOOT held opens the menu scrolled to its end (the DIAL group with Settings)'
assert 'menu_clamp(' in function('settings_end_offset') and 'TAB_ROWS' in function('settings_end_offset')
assert len(re.findall(r'view\(SETTINGS\)', ui)) == 2, 'the menu has two entry points: BOOT held and the back from a Settings screen'
assert '"ui/habitat/menu_list.c"' in text(fw / 'main/CMakeLists.txt'), 'menu_list.c is built'
assert '#include "menu_list.h"' in ui

# --- the production table, compiled ---------------------------------------------------------------------------------------------------------------
# The expected lines come from an independent rule written here in Python (group by group, a label before a group's first visible row, rows in table order),
# not from menu_list.c, so a mistake in the model or in the table's use of it shows. A row is a letter ('a' + its table index), a label is 'L'.
actions = sorted(set(re.findall(r'\bA_[A-Z_]+\b', table + settings_functions)))
table_text = re.search(r'static const struct \{[^}]*\} menu_items\[\] = \{.*?\n\};', ui, re.S).group(0)
all_rows = re.findall(r'\{"([^"]+)",\s*(A_\w+),\s*([A-Za-z_0-9]+),\s*(MENU_GROUP_\w+)\}', table_text)
FEATURES = ['CABLE_FEATURE_FORM', 'CABLE_FEATURE_SELECTION', 'CABLE_FEATURE_VISIT', 'CABLE_FEATURE_RUNNING']


def expected(have):
    out, last = '', None
    for i, (label, action, feature, group) in enumerate(all_rows):
        if feature != '0' and feature not in have:
            continue
        if group != last:
            out += 'L'
            last = group
        out += chr(ord('a') + i)
    return out


cases = [set(), {'CABLE_FEATURE_FORM'}, {'CABLE_FEATURE_FORM', 'CABLE_FEATURE_SELECTION', 'CABLE_FEATURE_VISIT'}, {'CABLE_FEATURE_RUNNING'}, set(FEATURES)]
checks = ''
for have in cases:
    pic = expected(have)
    n = len(pic)
    visible = [all_rows[ord(c) - ord('a')] for c in pic if c != 'L']
    mask = ' | '.join(sorted(have)) or '0'
    checks += '    host_mask = %s;\n    picture(pic);\n    assert(!strcmp(pic, "%s") && settings_count() == %d);\n' % (mask, pic, n)
    checks += '    assert(settings_end_offset() == %d);\n' % (n - 4 if n > 4 else 0)
    for k, row in enumerate(visible):
        checks += '    assert(settings_item(%d, &a) && a == %s && !strcmp(settings_item(%d, NULL), "%s"));\n' % (k, row[1], k, row[0])
    checks += '    assert(!settings_item(%d, NULL) && !settings_item(-1, NULL));\n' % len(visible)
harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cable_features.h"
#include "menu_list.h"
#define TAB_ROWS 4
typedef enum { A_NONE, """ + ', '.join(actions) + r""" } action_kind_t;
static uint32_t host_mask;
bool cable_client_supports(uint32_t features) { return (host_mask & features) == features; }
""" + table_text + "\n" + "\n".join(function(n) for n in ('settings_lines', 'settings_item', 'settings_count', 'settings_end_offset')) + r"""
static void picture(char *out)
{
    menu_line_t l[MENU_MAX_LINES];
    int n = settings_lines(l);
    for (int i = 0; i < n; i++) out[i] = l[i].item < 0 ? 'L' : (char)('a' + l[i].item);
    out[n] = 0;
}
int main(void)
{
    char pic[32];
    action_kind_t a;
""" + checks + r"""    // a feature that arrives later: the kept offset is clamped by the model
    menu_line_t l[MENU_MAX_LINES];
    host_mask = 0;
    int n = settings_lines(l);
    assert(menu_clamp(l, n, TAB_ROWS, 9) == settings_end_offset() && menu_clamp(l, n, TAB_ROWS, 0) == 0);
    (void)a;
    puts("menu groups: production table, settings_item and settings_count agree with the model");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='harness-menu-groups-') as path:
    out = Path(path)
    (out / 'groups.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(fw / 'main'), '-I', str(habitat), str(out / 'groups.c'), str(habitat / 'menu_list.c'), '-o', str(out / 'groups')], check=True)
    subprocess.run([str(out / 'groups')], check=True)
print('menu groups: ten-row table in three groups, MENU heading, labels without hits, scroll by the model, BOOT opens at the end, production helpers compiled PASS')
