"""The running marker and the empty text (dial-reach T3), and the page model's tie to the daemon (T6).

Source rules on comment-stripped text (the style of test_menu_screen.py). The behaviour of the reader itself (agents.end -> snapshot.running)
is run in test_agent_snapshot.py against the production functions.

  * send_hello says `running: "v1"` on every transport (the daemon then builds the no-window list);
  * agents.end sets s_agents_running from the frame's `running` key under the lock, session_down clears it, the snapshot copies it;
  * ui_fleet_set takes the third argument everywhere it is declared, defined and called;
  * with s.running and no agent the Focus face says "No running" / "agents." and draws NO "Choose a tab" pill and registers no hit for it; the
    legacy face says "No running agents." and no pill; s.running false keeps the old text and the pill;
  * the word "harnesses" is on no screen text (C8: an agent is an "agent").
"""
from pathlib import Path
import re

here = Path(__file__).resolve().parent
fw = here / '..'
main = fw / 'main'
habitat = main / 'ui/habitat'


def text(path):
    return Path(path).read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def code(path):
    src = re.sub(r'/\*.*?\*/', ' ', text(path), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def function(name, src):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, 'no function ' + name
    return match.group(0)


client = code(main / 'cable_client.c')
header = code(main / 'cable_client.h')
screens = code(main / 'ui/ui_screens.h')
ui = code(habitat / 'ui_habitat.c')
app = code(main / 'app_main.c')
perf = code(main / 'ui/ui_perf.c')

# --- the wire -------------------------------------------------------------------------------------------------------------------------------
hello = function('send_hello', client)
assert re.search(r'msg_string\(&root,\s*"running",\s*"v1"\)', hello), 'hello must say running v1'
lines = [l.strip() for l in hello.splitlines() if l.strip()]
at = next(i for i, l in enumerate(lines) if '"running"' in l)
assert not re.match(r'(if\b|else\b|#)', lines[at - 1]) and not lines[at].startswith('if'), 'running v1 is sent on every transport, not behind an if'

end = function('handle_agents_end', client)
assert re.search(r'cJSON_GetObjectItemCaseSensitive\(p,\s*"running"\)', end) and 'cJSON_IsTrue' in end, 'agents.end reads the running key'
assert re.search(r's_agents_running\s*=', end), 'agents.end sets s_agents_running'
assert end.index('xSemaphoreTake') < end.index('s_agents_running =') < end.rindex('xSemaphoreGive'), 'under the lock'
assert '(running)' in end, 'the log line says when the marker is set'

down = function('session_down', client)
assert re.search(r's_agents_running\s*=\s*false', down), 'session_down clears the marker'

snap = function('cable_client_list_agents_snapshot', client)
assert re.search(r'snapshot->running\s*=\s*s_agents_running', snap), 'the snapshot carries the marker'
assert re.search(r'bool\s+running;', header), 'cable_agent_snapshot_t.running'

# --- ui_fleet_set(total, window, running) -----------------------------------------------------------------------------------------------------
assert re.search(r'void ui_fleet_set\(int total, bool has_window, bool running\);', screens), 'declaration'
assert re.search(r'^void ui_fleet_set\(int total, bool window, bool running\)', ui, re.M), 'definition'
assert re.search(r's\.running\s*=\s*running;', function('ui_fleet_set', ui)), 'ui_fleet_set stores it'
assert 'ui_fleet_set(snapshot.total, snapshot.window, snapshot.running);' in app, 'app_main passes it'
calls = re.findall(r'ui_fleet_set\(([^;]*)\);', perf)
assert len(calls) == 2 and all(c.rstrip().endswith('false') and c.count(',') == 2 for c in calls), 'ui_perf passes false twice: %r' % calls
assert not re.search(r'ui_fleet_set\([^,;]*,[^,;]*\)', app + perf + ui), 'no two-argument call is left'

# --- the empty text -------------------------------------------------------------------------------------------------------------------------
panes = function('render_focus_panes', ui)
assert '"No running"' in panes and '"agents."' in panes, 'Focus face: two lines'
assert 's.running' in panes
pill = panes.index('A_TABS')
assert re.search(r'const bool running\s*=\s*s\.running\s*&&\s*!here;', panes), 'running is the marker, outside a swarm'
assert re.search(r'if\s*\(\s*running\s*\)\s*return;', panes[:pill]), 'the Choose-a-tab pill (and its A_TABS hit) is skipped when running'
assert '"Choose a tab"' in panes, 'a window with an empty tab keeps its own empty text and pill'

agents = function('render_agents', ui)
assert '"No running agents."' in agents, 'legacy face'
legacy_pill = agents[agents.index('"No running agents."'):]
assert 'A_TABS' in legacy_pill and '!s.running' in legacy_pill, 'the legacy pill is guarded too'

assert 'harnesses' not in panes and 'harnesses' not in agents, 'C8: agents, not harnesses'
assert not re.search(r'"[^"\n]*[Hh]arnesses[^"\n]*"', ui + client), 'no on-screen text says harnesses'
print('running marker and empty text: hello, reader, snapshot, ui_fleet_set arity, both faces, no pill, wording PASS')
