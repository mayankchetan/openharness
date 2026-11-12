"""One word set on the dial (dial-reach T9, plan amendment B1, rule C8).

An agent is an "agent", a desktop workspace is a "tab", a machine is a "computer". The word "Harness" stays only where it means the app. This covers
the text the DIAL owns; text that the daemon or the desktop sends is out of scope, except the two form titles "Find Harness" / "New Harness", which
the dial maps for display (form_title_shown).

Source rules on comment-stripped text (the style of test_menu_screen.py), plus one compiled check of form_title_shown from the production source:

  (a) every new string of the word map exists in its file (with its count);
  (b) every old on-screen string is gone from ui_habitat.c, focus.c, form.c and cable_client.c (the two desktop form titles are protocol values and
      may stay only where the dial compares against them);
  (c) no string literal in an on-screen sink (COPY(s.title / s.message / s.voice_target, heading, focus_header, center, control, text, ui_show_error,
      the RESTING and RELAXING lists ...) holds pane, workspace, machine or harnesses, and "Harness" only in the allow-list;
  (d) form_title_shown: "Find Harness" -> "Find agent", "New Harness" -> "New agent", "" by `finding`, any other title (an agent label while picking)
      is returned unchanged.
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


FILES = {
    'ui_habitat.c': code(habitat / 'ui_habitat.c'),
    'focus.c': code(habitat / 'focus.c'),
    'form.c': code(habitat / 'form.c'),
    'cable_client.c': code(fw / 'main/cable_client.c'),
}
ui = FILES['ui_habitat.c']


def literal(s):
    return '"' + s + '"'


def count(name, s):
    return FILES[name].count(literal(s))


# --- (a) the new strings -------------------------------------------------------------------------------------------------------------------
NEW = [
    # (file, string, minimum count)
    ('ui_habitat.c', 'Tabs', 1),                                    # workspace_failed's title
    ('ui_habitat.c', 'Opening your tab...', 2),                     # workspace_failed's sibling and the tab switch
    ('ui_habitat.c', 'tabs', 2),                                    # the tab preview heading, both skins
    ('ui_habitat.c', 'Choose an agent', 1),
    ('ui_habitat.c', 'AGENTS', 1),
    ('ui_habitat.c', 'No agents in', 1),
    ('ui_habitat.c', 'No agents in this tab.', 1),
    ('ui_habitat.c', 'agents', 1),                                  # the terminal heading
    ('ui_habitat.c', 'computers', 1),
    ('ui_habitat.c', 'Find agent', 2),                              # the form fallback and the voice target
    ('ui_habitat.c', 'New agent', 2),                               # the form fallback and the message title
    ('ui_habitat.c', 'Open Harness on your computer to start a new agent.', 1),
    ('ui_habitat.c', 'Your previous agent', 1),
    ('ui_habitat.c', 'That tab is gone. Choose another.', 1),
    ('ui_habitat.c', 'Computer', 1),                                # the error title
    ('ui_habitat.c', 'The computer did not answer. Please try again.', 1),
    ('ui_habitat.c', 'Waiting for that agent...', 1),
    ('ui_habitat.c', 'Link this computer', 1),
    ('ui_habitat.c', 'Pair computer', 1),
    ('ui_habitat.c', 'Computer paired', 1),
    ('ui_habitat.c', '[ on computer ]', 2),                         # D9: both question screens
    ('ui_habitat.c', 'agent', 1),                                   # the voice target's fallback
    ('ui_habitat.c', 'Agent', 3),                                   # the fallback agent name (selection, notice, draft)
    ('focus.c', 'Tap name to switch agents', 1),
    ('form.c', 'No reply. Check Harness on your computer.', 1),
    ('cable_client.c', 'Computer', 1),                              # the default computer name
]
focus = FILES['focus.c']
# "Tap name for agents" is one letter longer than the old line; if it does not fit the arc (test_character.c measures it) the shorter "Tap for agents" is used.
assert count('focus.c', 'Tap name for agents') + count('focus.c', 'Tap for agents') >= 1, 'the relaxing face teaches tapping the name for agents'
for name, string, minimum in NEW:
    assert count(name, string) >= minimum, '%s: "%s" x%d expected, found %d' % (name, string, minimum, count(name, string))

# --- (b) the old strings are gone ----------------------------------------------------------------------------------------------------------
OLD = [
    ('ui_habitat.c', 'Workspaces'), ('ui_habitat.c', 'Opening your workspace...'), ('ui_habitat.c', 'workspaces'),
    ('ui_habitat.c', 'Choose a pane'), ('ui_habitat.c', 'PANES'), ('ui_habitat.c', 'No panes in'), ('ui_habitat.c', 'No panes in this tab.'),
    ('ui_habitat.c', 'panes'), ('ui_habitat.c', 'machines'), ('ui_habitat.c', 'Your previous pane'),
    ('ui_habitat.c', 'That workspace is gone. Choose another.'), ('ui_habitat.c', 'Machine'), ('ui_habitat.c', 'The machine did not answer. Please try again.'),
    ('ui_habitat.c', 'Waiting for that pane...'), ('ui_habitat.c', 'Link this machine'), ('ui_habitat.c', 'Pair machine'), ('ui_habitat.c', 'Machine paired'),
    ('ui_habitat.c', '[ on desktop ]'), ('ui_habitat.c', 'Harness'),  # "Harness" alone: only the brand screen (3 draws) and the two toast titles remain
    ('ui_habitat.c', 'harness'), ('ui_habitat.c', 'Open New Harness on your computer.'),
    ('focus.c', 'Tap the name to switch panes'), ('focus.c', 'Tap the name to switch agents'), ('focus.c', 'Tap name for panes'),
    ('form.c', 'No reply. Check Harness on desktop.'),
    ('cable_client.c', 'Machine'),
]
for name, string in OLD:
    n = count(name, string)
    if (name, string) == ('ui_habitat.c', 'Harness'):
        assert n == 5, 'the brand screen (3) and the two toast titles (2) keep "Harness"; found %d' % n
    elif (name, string) == ('ui_habitat.c', 'machines') or (name, string) == ('ui_habitat.c', 'panes'):
        # the view names of habitat_view_name are log names, not screen text (test_ui_gallery_items.c pins them)
        assert re.findall(r'return\s+"%s";' % string, FILES[name]) and n == len(re.findall(r'return\s+"%s";' % string, FILES[name])), \
            '"%s" may stay only as a view name in habitat_view_name; found %d' % (string, n)
    else:
        assert n == 0, '%s: the old string "%s" is still there (%d)' % (name, string, n)
# the desktop's own titles are protocol values: they stay only where the dial compares against them
body = re.search(r'form_title_shown\([^;{]*\)\n\{.*?^\}', ui, re.M | re.S)
assert body, 'form_title_shown exists'
for protocol in ('Find Harness', 'New Harness'):
    for m in re.finditer(re.escape(literal(protocol)), ui):
        line = ui[ui.rfind('\n', 0, m.start()) + 1:ui.find('\n', m.end())]
        assert 'strcmp' in line, 'the desktop title "%s" is only ever compared, never drawn: %s' % (protocol, line.strip())

# --- (c) the words in the on-screen sinks --------------------------------------------------------------------------------------------------
SINKS = ['COPY(s.title', 'COPY(s.message', 'COPY(s.voice_target', 'heading(', 'focus_header(', 'center(', 'control(', 'text(', 'ui_wrap(',
         'ui_show_error(', 'command_face(', 'focus_put(', 'snprintf(s.message', 'snprintf(s.voice_target', 'snprintf(s.title', 'snprintf(label']


def literals_of(src, at):
    """The string literals inside the balanced parentheses that start at src[at] == '('."""
    depth, i, out = 0, at, []
    while i < len(src):
        ch = src[i]
        if ch == '"':
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i + 1:j])
            i = j
        elif ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return out
        i += 1
    return out


def sink_literals(src):
    found = []
    for sink in SINKS:
        for m in re.finditer(r'(?<![A-Za-z_0-9])' + re.escape(sink), src):
            open_at = src.index('(', m.start())
            found += literals_of(src, open_at)
    return found


BANNED = re.compile(r'\b(panes?|workspaces?|machines?|harnesses)\b', re.I)
ALLOWED_HARNESS = [r'^Harness$', r'Check Harness on your computer\.', r'Open Harness on your computer to start a new agent\.', r'harness link (create|import)',
                   r'harness dial wifi set']
checked = 0
for name, src in FILES.items():
    # A strcmp against the desktop's own title is a protocol comparison, not text on the glass.
    lits = sink_literals(re.sub(r'strcmp\([^()]*\)', '0', src))
    if name == 'focus.c':   # the two resting lists and their #defines
        lits += re.findall(r'#define\s+(?:HOLD|TAP)\w*\s+"([^"]*)"', src)
    for lit in lits:
        checked += 1
        assert not BANNED.search(lit), '%s: on-screen text "%s" holds an old word (pane, workspace, machine or harnesses)' % (name, lit)
        if re.search(r'harness', lit, re.I):
            assert any(re.search(a, lit) for a in ALLOWED_HARNESS), '%s: on-screen text "%s" says Harness outside the allow-list' % (name, lit)
assert checked > 100, 'the sink scan found only %d literals; it is not scanning' % checked

# --- (d) form_title_shown, compiled from the production source -----------------------------------------------------------------------------
assert re.search(r'form_title_shown\(p->title,\s*finding\)', ui), 'render_form draws the mapped title'
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
''' + re.search(r'^static const char \*form_title_shown\([^;{]*\)\n\{.*?^\}', ui, re.M | re.S).group(0) + r'''
int main(void)
{
    assert(!strcmp(form_title_shown("Find Harness", true), "Find agent"));
    assert(!strcmp(form_title_shown("Find Harness", false), "Find agent"));
    assert(!strcmp(form_title_shown("New Harness", false), "New agent"));
    assert(!strcmp(form_title_shown("", true), "Find agent"));
    assert(!strcmp(form_title_shown("", false), "New agent"));
    assert(!strcmp(form_title_shown(NULL, true), "Find agent"));
    assert(!strcmp(form_title_shown("Fix login screen", true), "Fix login screen"));   // an agent label while picking: unchanged (Review Focus 11)
    assert(!strcmp(form_title_shown("Find Harness 2", true), "Find Harness 2"));
    assert(!strcmp(form_title_shown("find harness", true), "find harness"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-word-set-') as path:
    out = Path(path)
    (out / 'title.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    str(out / 'title.c'), '-o', str(out / 'title')], check=True)
    subprocess.run([str(out / 'title')], check=True)
print('word set: new strings present, old strings gone, %d on-screen literals clean, form titles mapped for display only PASS' % checked)
