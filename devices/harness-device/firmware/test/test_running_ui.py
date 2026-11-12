"""The "All agents" view and its wire (dial-reach T7).

A menu row (ON COMPUTER group, first, gated by the daemon's `running.list` feature) opens a paged list of the RUNNING agents of this computer: four rows a
page, each with its status. A tap on a row sends `agent.open` (no reason) for that agent and goes home; the daemon moves the desktop or, with no window, the
dial's own list. The dial never adds or removes a tile by itself. A drag pages: one `running.get {offset, request}` a step, and a reply for any request but the
latest is dropped (cable_running.c, test_cable_running.c). Nothing refreshes on a timer: leaving and re-entering the view asks again.

Three kinds of check:
  * handle_running_page (cable_client.c) compiled against ESP-IDF's real cJSON (IDF_PATH): good and bad frames;
  * the production view logic of ui_habitat.c (ui_running_replace, ui_running_clear, running_open, running_ask, running_pick, running_drag,
    running_empty_text) compiled with fakes: stale replies, the pick, paging, the empty and loading text;
  * source rules on comment-stripped text: the menu row and the 14-line list (compiled from the production table), PSRAM placement, the gesture lists,
    the words, the wire function, the worker cases.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
fw = here / '..'
main = fw / 'main'
habitat = main / 'ui/habitat'


def text(path):
    return Path(path).read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def code(path):
    src = re.sub(r'/\*.*?\*/', ' ', text(path), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


ui = code(habitat / 'ui_habitat.c')
client = code(main / 'cable_client.c')
client_raw = text(main / 'cable_client.c')
ui_raw = text(habitat / 'ui_habitat.c')


def function(name, src):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, 'no definition of ' + name
    return match.group(0) + '\n'


san = '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds')

# --- 1. handle_running_page against the real cJSON ------------------------------------------------------------------------------------------------
if 'IDF_PATH' in os.environ:
    json_dir = Path(os.environ['IDF_PATH']) / 'components/json/cJSON'
    page = r'''
#include "cJSON.h"
#include "cable_running.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ID_MAX 48
#define EXT_RAM_BSS_ATTR
static int replaced, last_n, last_offset, last_total;
static uint32_t last_request;
static cable_running_row_t last_rows[CABLE_RUNNING_ROWS];
void ui_running_replace(uint32_t request, int offset, int total, const cable_running_row_t *rows, int n)
{
    replaced++; last_request = request; last_offset = offset; last_total = total; last_n = n;
    for (int i = 0; i < n; i++) last_rows[i] = rows[i];
}
''' + function('whole_number', client) + function('handle_running_page', client) + r'''
static int feed(const char *json)
{
    cJSON *p = cJSON_Parse(json);
    assert(p);
    int before = replaced;
    handle_running_page(p);
    cJSON_Delete(p);
    return replaced - before;
}
int main(void)
{
    // a valid four-row page: one call, with every field
    assert(feed("{\"t\":\"running.page\",\"request\":7,\"offset\":4,\"total\":10,\"rows\":["
                "{\"id\":\"a1\",\"name\":\"Fix login\",\"engine\":\"claude\",\"machineId\":\"m1\",\"status\":\"working\"},"
                "{\"id\":\"a2\",\"name\":\"Docs\",\"engine\":\"codex\",\"machineId\":\"m1\",\"status\":\"question\"},"
                "{\"id\":\"a3\",\"name\":\"\",\"engine\":\"pi\",\"machineId\":\"m1\",\"status\":\"idle\"},"
                "{\"id\":\"a4\",\"name\":\"Zed\",\"engine\":\"terminal\",\"machineId\":\"m1\",\"status\":\"mystery\"}]}") == 1);
    assert(last_request == 7 && last_offset == 4 && last_total == 10 && last_n == 4);
    assert(!strcmp(last_rows[0].id, "a1") && !strcmp(last_rows[0].name, "Fix login") && !strcmp(last_rows[0].engine, "claude") && last_rows[0].status == RUNNING_WORKING);
    assert(last_rows[1].status == RUNNING_NEEDS_YOU && last_rows[2].status == RUNNING_IDLE && last_rows[3].status == RUNNING_IDLE);
    assert(!strcmp(last_rows[2].name, "") && !strcmp(last_rows[3].engine, "terminal"));
    // an empty list is a page too
    assert(feed("{\"request\":1,\"offset\":0,\"total\":0,\"rows\":[]}") == 1 && last_n == 0 && last_total == 0);
    // five rows are rejected, whole
    assert(feed("{\"request\":2,\"offset\":0,\"total\":9,\"rows\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"},{\"id\":\"d\"},{\"id\":\"e\"}]}") == 0);
    // an id of 48 bytes (ID_MAX) is rejected, 47 is accepted
    char id48[49], id47[48], big[400];
    memset(id48, 'x', 48); id48[48] = 0; memset(id47, 'y', 47); id47[47] = 0;
    snprintf(big, sizeof big, "{\"request\":3,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"%s\",\"name\":\"n\"}]}", id48);
    assert(feed(big) == 0);
    snprintf(big, sizeof big, "{\"request\":3,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"%s\",\"name\":\"n\"}]}", id47);
    assert(feed(big) == 1 && !strcmp(last_rows[0].id, id47));
    // a missing or bad request is rejected
    assert(feed("{\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":0,\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":-1,\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":2147483648,\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":1.5,\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":\"3\",\"offset\":0,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":2147483647,\"offset\":0,\"total\":0,\"rows\":[]}") == 1 && last_request == 2147483647u);
    // offset and total
    assert(feed("{\"request\":1,\"offset\":-1,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":1,\"offset\":1000001,\"total\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":1,\"rows\":[]}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"rows\":[]}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":-5,\"rows\":[]}") == 0);
    // rows that run past the total reach the model, which refuses them (cable_running_apply, test_cable_running.c): the handler passes the numbers on as they are
    // rows
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":{}}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[5]}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"name\":\"no id\"}]}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"\"}]}") == 0);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"id\":7}]}") == 0);
    // a name over 95 bytes is cut, never rejected; an engine over 15 likewise; a non-string name is empty
    char name120[121], json[600];
    memset(name120, 'n', 120); name120[120] = 0;
    snprintf(json, sizeof json, "{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"a\",\"name\":\"%s\",\"engine\":\"engine-name-too-long\"}]}", name120);
    assert(feed(json) == 1 && strlen(last_rows[0].name) == 95 && strlen(last_rows[0].engine) == 15);
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"a\",\"name\":5,\"engine\":null,\"status\":3}]}") == 1 &&
           !last_rows[0].name[0] && !last_rows[0].engine[0] && last_rows[0].status == RUNNING_IDLE);
    // multibyte names stay as they are up to the cut (a cut inside a letter is the display's business: it measures by codepoint)
    assert(feed("{\"request\":1,\"offset\":0,\"total\":1,\"rows\":[{\"id\":\"a\",\"name\":\"\\u00e9\\u00e9\"}]}") == 1 && !strcmp(last_rows[0].name, "\xc3\xa9\xc3\xa9"));
    puts("handle_running_page: good frames, five rows, id of 48 bytes, bad request/offset/total/rows, long names PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='harness-running-page-') as path:
        out = Path(path)
        (out / 'page.c').write_text(page)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-O1', '-g', san, '-I', str(json_dir), '-I', str(main),
                        str(out / 'page.c'), str(main / 'cable_running.c'), str(json_dir / 'cJSON.c'), '-o', str(out / 'page'), '-lm'], check=True)
        subprocess.run([str(out / 'page')], check=True)
else:
    print('IDF_PATH is not set: the cJSON half of test_running_ui.py is skipped')

# --- 2. the view logic of ui_habitat.c, with fakes -----------------------------------------------------------------------------------------------
logic = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cable_running.h"
#define ID_MAX 48
enum { HOME, SETTINGS, RUNNING };
typedef enum { A_NONE, A_DESKTOP, A_RUNNING_GET } action_kind_t;
typedef struct { action_kind_t kind; int value; char id[ID_MAX]; char text[192]; uint32_t revision; int dy, velocity; } action_t;
static struct { cable_running_t running_page; int running_want, view, offset, list_offset; } s;
static int locks, changes, views, queued, queue_fail;
static action_t sent[32];
static void display_lock(void) { locks++; }
static void display_unlock(void) { assert(locks > 0); locks--; }
static void change(void) { assert(locks > 0); changes++; }
static void view(int v) { s.view = v; views++; }
static bool queue(action_t a) { if (queue_fail || queued >= 32) return false; sent[queued++] = a; return true; }
''' + function('copy', ui) + '#define COPY(dst, src) copy(dst, sizeof(dst), src)\n' + ''.join(function(n, ui) for n in ('running_empty_text', 'ui_running_replace', 'ui_running_clear', 'running_ask', 'running_open', 'running_pick', 'running_drag')) + r'''
static void rows_of(cable_running_row_t *r, int n, int first)
{
    memset(r, 0, n * sizeof *r);
    for (int i = 0; i < n; i++) { snprintf(r[i].id, sizeof r[i].id, "id-%d", first + i); snprintf(r[i].name, sizeof r[i].name, "name %d", first + i); r[i].status = (running_status_t)(i % 3); }
}
int main(void)
{
    cable_running_row_t rows[4];
    cable_running_init(&s.running_page);
    s.view = SETTINGS; s.offset = 6;
    // before the first page there is nothing to draw but a word
    assert(!strcmp(running_empty_text(&s.running_page), "Loading..."));
    running_open();
    assert(s.view == RUNNING && s.list_offset == 6 && queued == 1 && sent[0].kind == A_RUNNING_GET && sent[0].value == 0 && sent[0].revision == 1);
    assert(!strcmp(running_empty_text(&s.running_page), "Loading..."));
    // a reply for another request is dropped and the shown page stays
    rows_of(rows, 4, 0);
    ui_running_replace(99, 0, 10, rows, 4);
    assert(!s.running_page.loaded && changes == 0 && locks == 0);
    // the latest reply lands: repaint once
    ui_running_replace(1, 0, 10, rows, 4);
    assert(s.running_page.loaded && s.running_page.count == 4 && s.running_page.total == 10 && changes == 1 && locks == 0);
    assert(running_empty_text(&s.running_page) == NULL);
    // an older reply arriving after a newer one changes nothing
    running_drag(1);   // asks for offset 1
    assert(queued == 2 && sent[1].value == 1 && sent[1].revision == 2 && !s.running_page.loaded);
    rows_of(rows, 4, 1);
    ui_running_replace(1, 0, 10, rows, 4);   // the old request's late reply
    assert(!s.running_page.loaded && s.running_page.offset == 0);
    // a second step before the reply asks from the pending offset, not from the shown one
    running_drag(1);
    assert(queued == 3 && sent[2].value == 2 && sent[2].revision == 3);
    running_drag(100);
    assert(queued == 4 && sent[3].value == 6 && sent[3].revision == 4);   // clamped at total - 4
    rows_of(rows, 4, 6);
    int before = changes;
    ui_running_replace(4, 6, 10, rows, 4);
    assert(s.running_page.loaded && s.running_page.offset == 6 && changes == before + 1);
    // at the end a further step asks for nothing
    running_drag(1);
    running_drag(50);
    assert(queued == 4);
    // back up
    running_drag(-2);
    assert(queued == 5 && sent[4].value == 4 && sent[4].revision == 5);
    // the daemon may clamp: the reply's own offset is the one that counts
    rows_of(rows, 3, 7);
    ui_running_replace(5, 7, 10, rows, 3);
    assert(s.running_page.offset == 7 && s.running_page.count == 3 && s.running_page.loaded);
    // a lost reply: the next drag asks again even at the same target
    running_drag(-3);   // target 4 again, pending
    assert(queued == 6 && !s.running_page.loaded);
    running_drag(0);
    assert(queued == 7 && sent[6].value == 4);   // not loaded: asked again
    // pick: agent.open for that row's id, once, then home
    rows_of(rows, 4, 0);
    ui_running_replace(s.running_page.request, 0, 4, rows, 4);
    int q = queued, v = views;
    running_pick(2);
    assert(queued == q + 1 && sent[q].kind == A_DESKTOP && !strcmp(sent[q].id, "id-2") && s.view == HOME && views == v + 1);
    running_pick(4); running_pick(-1); running_pick(99);
    assert(queued == q + 1);
    s.running_page.rows[1].id[0] = 0;
    running_pick(1);
    assert(queued == q + 1);   // a row with no id sends nothing
    // a full queue: no view change on a failed send is not required, but no crash and nothing queued twice
    s.view = RUNNING; queue_fail = 1; q = queued; running_pick(0); queue_fail = 0;
    assert(queued == q);
    // no agents: after a page with total 0 the empty text
    s.view = RUNNING;
    running_ask(0);
    ui_running_replace(s.running_page.request, 0, 0, rows, 0);
    assert(s.running_page.loaded && s.running_page.total == 0 && !strcmp(running_empty_text(&s.running_page), "No running agents"));
    running_drag(1); running_drag(-1);
    // the session goes down: everything is forgotten, and the model is not loaded
    changes = 0;
    ui_running_clear();
    assert(!s.running_page.loaded && s.running_page.count == 0 && s.running_page.request == 0 && s.running_want == 0 && changes == 1 && locks == 0);
    s.view = HOME; changes = 0;
    ui_running_clear();
    assert(changes == 0);   // not on screen: no repaint
    puts("running view: stale replies dropped, one repaint a page, paging from the pending offset, clamp at both ends, pick sends agent.open once, empty and loading text, clear PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-running-ui-') as path:
    out = Path(path)
    (out / 'logic.c').write_text(logic)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-O1', '-g', san, '-I', str(main), str(out / 'logic.c'), str(main / 'cable_running.c'),
                    '-o', str(out / 'logic')], check=True)
    subprocess.run([str(out / 'logic')], check=True)

# --- 3. source rules ----------------------------------------------------------------------------------------------------------------------------
# the menu row: first in ON COMPUTER (before Find agent), gated by the daemon's feature
table = re.search(r'menu_items\[\] = \{.*?\n\};', ui, re.S).group(0)
rows = re.findall(r'\{"([^"]+)", (A_\w+), ([A-Za-z_0-9]+), (MENU_GROUP_\w+)\}', table)
names = [r[0] for r in rows]
assert ('All agents', 'A_RUNNING', 'CABLE_FEATURE_RUNNING', 'MENU_GROUP_COMPUTER') in rows, 'the All agents row'
assert names.index('All agents') == names.index('Find agent') - 1 == 3, 'first in ON COMPUTER, before Find agent'
assert [r[3] for r in rows[:3]] == ['MENU_GROUP_AGENT'] * 3 and rows[3][3] == 'MENU_GROUP_COMPUTER'
assert len(rows) == 11

# the production table, compiled: the 14-line list with the row on line 5, and nothing else changes without the bit
actions = sorted(set(re.findall(r'\bA_[A-Z_]+\b', table + function('settings_lines', ui) + function('settings_item', ui))))
table_text = re.search(r'static const struct \{[^}]*\} menu_items\[\] = \{.*?\n\};', ui, re.S).group(0)
menu = r"""
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
""" + table_text + '\n' + function('settings_lines', ui) + function('settings_item', ui) + function('settings_count', ui) + function('settings_end_offset', ui) + r"""
int main(void)
{
    menu_line_t l[MENU_MAX_LINES];
    action_kind_t a;
    host_mask = CABLE_FEATURE_FORM | CABLE_FEATURE_SELECTION | CABLE_FEATURE_VISIT | CABLE_FEATURE_RUNNING;
    int n = settings_lines(l);
    assert(n == 14 && settings_count() == 14);
    // L012 L3456 L789(10)  -> labels at 0, 4 and 9; the All agents row is line 5
    assert(l[0].item == -1 && l[4].item == -1 && l[9].item == -1);
    assert(l[5].item == 3 && !strcmp(settings_item(3, &a), "All agents") && a == A_RUNNING && !strcmp(settings_item(4, NULL), "Find agent"));
    assert(settings_end_offset() == 10);
    // the daemon says no running.list: the row goes and nothing else moves
    host_mask = CABLE_FEATURE_FORM | CABLE_FEATURE_SELECTION | CABLE_FEATURE_VISIT;
    n = settings_lines(l);
    assert(n == 13 && !strcmp(settings_item(3, NULL), "Find agent") && l[4].item == -1 && l[8].item == -1);
    // with only the connection (no optional feature but running.list, as with no window): ON COMPUTER holds just the row
    host_mask = CABLE_FEATURE_RUNNING;
    n = settings_lines(l);
    assert(n == 10 && l[3].item == -1 && l[4].item == 3 && l[5].item == -1);
    // an old daemon: no ON COMPUTER group at all, 8 lines
    host_mask = 0;
    assert(settings_lines(l) == 8);
    puts("menu with All agents: 14 lines, the row first in ON COMPUTER, gone and nothing moved without running.list, 8 lines on an old daemon PASS");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='harness-running-menu-') as path:
    out = Path(path)
    (out / 'menu.c').write_text(menu)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', san, '-I', str(main), '-I', str(habitat), str(out / 'menu.c'),
                    str(habitat / 'menu_list.c'), '-o', str(out / 'menu')], check=True)
    subprocess.run([str(out / 'menu')], check=True)

# PSRAM: the model lives inside the EXT_RAM_BSS_ATTR struct, the reader's row scratch is its own PSRAM static
s_struct = re.search(r'static EXT_RAM_BSS_ATTR struct \{(.*?)\n\} s;', ui, re.S)
assert s_struct and 'cable_running_t running_page;' in s_struct.group(1), 'the model is inside the PSRAM struct s'
assert re.search(r'static EXT_RAM_BSS_ATTR cable_running_row_t \w+\[CABLE_RUNNING_ROWS\];', function('handle_running_page', client)), 'the reader parses into a PSRAM static, not its stack'
assert 'cable_running_row_t' not in re.sub(r'static EXT_RAM_BSS_ATTR cable_running_row_t', '', function('handle_running_page', client)).replace('const cable_running_row_t', ''), 'no row on the stack'

# the view, its action and the worker
assert re.search(r'\bRUNNING\b', ui[ui.index('typedef enum {'):ui.index('} view_t;')]), 'RUNNING is a view'
assert 'case RUNNING: return "running";' in ui, 'a view name for the log and the gallery'
for kind in ('A_RUNNING', 'A_RUNNING_PICK', 'A_RUNNING_GET'):
    assert re.search(r'\b' + kind + r'\b', ui[ui.index('A_NONE,'):ui.index('} action_kind_t;')]), kind + ' is an action'
dispatch = ui[ui.index('case A_RUNNING:'):]
assert 'running_open();' in dispatch[:400] and 'cable_client_supports(CABLE_FEATURE_RUNNING)' in dispatch[:400] and 's.connected' in dispatch[:400], 'open: connected and the feature'
assert re.search(r'case A_RUNNING_PICK:\s*running_pick\(a\.value\);', ui), 'pick'
assert re.search(r'case A_RUNNING_GET:\s*cable_client_request_running\(a\.value, a\.revision\);', ui), 'the worker sends the request'
assert re.search(r'case RUNNING:\s*render_running\(f\);', ui), 'drawn'
render = function('render_running', ui)
assert 'focus_header(f, "AGENTS")' in render and re.search(r'control\(f, 87, 67, 48, "<", A_SETTINGS, 1, true\)', render), 'heading AGENTS and the back to the menu'
assert 'A_RUNNING_PICK' in render and 'running_empty_text(' in render
assert '"Loading..."' in function('running_empty_text', ui) and '"No running agents"' in function('running_empty_text', ui)
row = function('running_row', ui)
assert '"working"' in row and '"needs you"' in row and '"idle"' in row and 'ht_fit_width(' in row, 'the status words, and the name fitted beside them'
assert not re.search(r'harness', render + row + function('running_empty_text', ui) + function('running_pick', ui), re.I), 'C8: agents, not harnesses'
assert 'cable_running_t running_page;' in ui and 'int running_want;' in ui
# nothing here touches the tiles (spec L5): no roster call from the view's code
for name in ('running_open', 'running_ask', 'running_pick', 'running_drag', 'ui_running_replace', 'ui_running_clear'):
    body = function(name, ui)
    for banned in ('ui_project_', 'pane_memory', 'cable_client_send_focus', 'cable_client_select', 'ui_workspace'):
        assert banned not in body, name + ' must not touch the roster: ' + banned

# every gesture list that holds AGENTS with SETTINGS and PREFS holds RUNNING too, and so do tabs_move and the picker lists
lists = re.findall(r'\([^()]*s\.view == AGENTS \|\| s\.view == SETTINGS \|\| s\.view == PREFS\)|s\.view == TABS \|\| s\.view == AGENTS \|\| s\.view == SETTINGS \|\| s\.view == PREFS\)', ui)
assert len(lists) == 0, 'the old three-way lists are gone: %r' % lists
assert len(re.findall(r's\.view == AGENTS \|\| s\.view == SETTINGS \|\| s\.view == PREFS \|\| s\.view == RUNNING', ui)) == 3, 'the two drag handlers and the release branch (which also names TABS before it)'
assert 's.view == TABS || s.view == AGENTS || s.view == SETTINGS || s.view == PREFS || s.view == RUNNING' in ui, 'the release branch'
assert re.search(r'if \(s\.view == RUNNING\) \{[^}]*running_drag\(step\)', function('tabs_move', ui)), 'tabs_move pages'
assert 'case MESSAGE: case RUNNING:' in ui, 'a picker for the mode switch'
assert re.search(r'bool ui_picker_is_open\(void\) \{[^}]*s\.view == RUNNING', ui), 'a picker for the voice and carousel gates'

# the wire: the request, the dispatch, the session-down clear
req = function('cable_client_request_running', client)
assert '"running.get"' in req and '"offset"' in req and '"request"' in req and 'CABLE_FEATURE_RUNNING' in req, 'running.get {offset, request} only to a daemon that offers it'
assert 'bool cable_client_request_running(int offset, uint32_t request);' in code(main / 'cable_client.h')
assert re.search(r'strcmp\(t, "running\.page"\) == 0\)\s*\{\s*handle_running_page\(p\);\s*return;\s*\}', client), 'the dispatch'
assert 'ui_running_clear();' in function('session_down', client), 'session down clears the page'
screens = code(main / 'ui/ui_screens.h')
assert 'void ui_running_replace(uint32_t request, int offset, int total, const cable_running_row_t *rows, int n);' in screens and 'void ui_running_clear(void);' in screens
assert '"AGENTS"' in ui and '"Loading..."' in ui and '"No running agents"' in ui

# the gallery covers both states
gallery = text(habitat / 'ui_gallery.inc')
items = text(habitat / 'ui_gallery_items.h')
assert 'SETUP(running)' in gallery and 'SETUP(running_empty)' in gallery
assert re.search(r'X\(running,\s+"running",\s+0, 2000, "running",\s+"render_running"', items) and re.search(r'X\(running_empty,\s+"running_empty",\s+0, 2000, "running",\s+"render_running"', items)
print('running ui: menu row and 14 lines, PSRAM placement, view, actions, worker, gesture lists, wire, session clear, gallery, words PASS')
