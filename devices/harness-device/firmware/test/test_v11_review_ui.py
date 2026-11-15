"""v1.1 whole-branch review fixes in ui_habitat.c (the verified findings of 2026-11-15).

  3. boot_voice carries the agent's text: it builds the action as the A_PET tap built it (value 3, the carried text) and then makes it a voice action.
  4. running_ask returns what queue() said; running_open does not overwrite the busy MESSAGE (the logic itself is run in test_running_ui.py).
  5. prefs_row: no label runs into its value in the terminal skin. The PRODUCTION prefs_row is compiled with a recording ht_text and the real font metrics,
     over every label and every value the Settings and All-agents rows can show.
  6. bright_drag_end does not return early while the sound change of Night mode is still pending.
  7. running_pick leaves a Swarm before it goes home (test_running_ui.py runs it); menu_label names the desk agent only outside a Swarm; the Swarm face draws
     the Quiet moon.
  8. The All agents heading is "ALL AGENTS" and fits the glass: its width from the real Inter 20 metrics, against the chord of the round glass at the title's row
     and the "<" control beside it.
"""
from pathlib import Path
import math
import os
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
fw = here / '..'
main = fw / 'main'
habitat = main / 'ui/habitat'
san = '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds')


def text(path):
    return Path(path).read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')


def code(path):
    src = re.sub(r'/\*.*?\*/', ' ', text(path), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


ui = code(habitat / 'ui_habitat.c')


def function(name, src=ui):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, 'no definition of ' + name
    return match.group(0) + '\n'


# --- 3. boot_voice carries the text ----------------------------------------------------------------------------------------------------------------
bv = function('boot_voice')
assert re.search(r'make_action\(\(hit_t\)\{\.action = carry\.active \? A_PET : A_VOICE\}\);\s*out->kind = A_VOICE;', bv), \
    'HOME and AGENT: build with A_PET while a carry is on (value 3, the carried text), then make it the voice action'

# --- 4. running_ask returns queue()'s answer --------------------------------------------------------------------------------------------------------
assert 'static bool running_ask(int offset)\n{' in ui, 'running_ask returns bool'
ra = function('running_ask')
assert 'return queue(' in ra, 'running_ask returns what queue() said'
ro = function('running_open')
assert 'if (!running_ask(0)) return;' in ro and ro.index('running_ask(0)') < ro.index('view(RUNNING)'), 'a refused ask keeps the busy MESSAGE'
rd = function('running_drag')
assert 'running_ask(target);' in rd

# --- 6. bright_drag_end -----------------------------------------------------------------------------------------------------------------------------
bd = function('bright_drag_end')
assert re.search(r'if \(s\.brightness == s\.bright_from && !\(settings_pending\.fields & UI_SETTING_MUTED\)\) return;', bd), \
    'a drag back to the start level still writes the sound change Night mode made'

# --- 7. Swarm ---------------------------------------------------------------------------------------------------------------------------------------
rp = function('running_pick')
assert 'swarm_leave();' in rp and rp.index('queue(open)') < rp.index('swarm_leave();') < rp.index('view(HOME);'), 'leave the Swarm after the send, before home'
ml = function('menu_label')
assert 'group == MENU_GROUP_AGENT && !swarm_mode() ? active() : NULL' in ml, 'in a Swarm the group names no desk agent'
rs = function('render_swarm_face')
assert 'if (s.nap) render_moon(f);' in rs and rs.index('ht_character_face(f, &character') < rs.index('if (s.nap) render_moon(f);'), 'the Swarm face draws the Quiet moon'
assert re.search(r'static void render_moon\(ht_scene_t \*f\);', ui) and ui.index('static void render_moon(ht_scene_t *f);') < ui.index('static void render_swarm_face(ht_scene_t *f)\n{'), 'render_moon is declared before the Swarm face'

# --- 8. the heading ---------------------------------------------------------------------------------------------------------------------------------
assert 'focus_header(f, "ALL AGENTS");' in function('render_running')

# --- 5 and 8, from the production code and the real metrics -----------------------------------------------------------------------------------------
defs = dict(re.findall(r'#define (LIST_HIT_X|LIST_HIT_W|SET_TEXT_X)\s+(\d+)', ui))
assert set(defs) == {'LIST_HIT_X', 'LIST_HIT_W', 'SET_TEXT_X'}
items = re.search(r'static const struct \{ const char \*label; action_kind_t action; \} items\[\] = \{(.*?)\n    \};', ui, re.S).group(1)
labels = re.findall(r'\{"([^"]+)", A_\w+\}', items) + re.findall(r'label = "([^"]+)"; value\[0\] = 0;', function('render_prefs'))
assert 'Unplugged link' in labels and len(labels) >= 10, labels
# The words each row can show on its right (prefs_value, ht_link_label, ht_timer_label): the label is tried against its own values only.
shown = {'Brightness': ['100%', '8%'], 'Sound': ['On', 'Off'], 'Unplugged link': ['Auto', 'Wi-Fi', 'Phone', 'USB only'],
         'Screen off': ['Auto', '30 s', '1 min', '2 min', '5 min'], 'Night mode': ['On', 'Off'], 'Battery saver': ['On', 'Off']}
pairs = [(label, value) for label in labels for value in shown.get(label, [''])]
assert ('Unplugged link', 'USB only') in pairs and ('Restart dial', '') in pairs
harness = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "terminal.h"
#include "focus_faces.h"
#define LIST_HIT_X ''' + defs['LIST_HIT_X'] + r'''
#define LIST_HIT_W ''' + defs['LIST_HIT_W'] + r'''
#define SET_TEXT_X ''' + defs['SET_TEXT_X'] + r'''
#define UI_FONT (&ht_mono_28)
typedef struct { int x, w; const ht_font_t *font; char text[HT_TEXT_BYTES]; } put_t;
static put_t puts_[8];
static int nputs;
static bool fake_text(ht_scene_t *f, int x, int y, int w, const ht_font_t *font, uint16_t fg, uint16_t bg, const char *t)
{
    (void)f; (void)y; (void)fg; (void)bg;
    assert(nputs < 8);
    puts_[nputs].x = x; puts_[nputs].w = w; puts_[nputs].font = font;
    snprintf(puts_[nputs].text, sizeof puts_[nputs].text, "%s", t);
    nputs++;
    return true;
}
#define ht_text fake_text
static bool focus_skin(void) { return false; }
static const ht_font_t *focus_face_for(const ht_font_t *m) { return m; }
static void focus_put(ht_scene_t *f, int x, int y, int w, const ht_font_t *font, uint16_t ink, uint16_t ground, const char *t, bool b)
{ (void)f; (void)x; (void)y; (void)w; (void)font; (void)ink; (void)ground; (void)t; (void)b; }
''' + function('prefs_row') + r'''
#undef ht_text
static int worst = 1 << 20;
static void row(const char *label, const char *value)
{
    nputs = 0;
    prefs_row(NULL, 0, label, value, 0, 0, 0);
    assert(nputs >= 1 && nputs <= 2);
    const int right = LIST_HIT_X + LIST_HIT_W - 16;   // the value's right edge: inside the row, as it always was
    const int lw = ht_measure(puts_[0].font, puts_[0].text);
    assert(puts_[0].x == SET_TEXT_X && lw <= puts_[0].w);   // the label is drawn in the box it was given
    assert(!strcmp(puts_[0].text, label));                   // and whole: the layout drops to the small face before it cuts a word of a Settings row
    if (nputs == 2) {
        assert(puts_[1].font == puts_[0].font);              // one face a row
        assert(puts_[1].x + ht_measure(puts_[1].font, puts_[1].text) <= right);
        const int gap = puts_[1].x - (puts_[0].x + lw);
        if (gap < worst) worst = gap;
        if (gap < 8) { printf("OVERLAP label '%s' (%s) ends at %d, value '%s' starts at %d\n", puts_[0].text, label, puts_[0].x + lw, value, puts_[1].x); assert(0); }
    } else {
        assert(puts_[0].x + lw <= right);   // no value: the label may use the row up to the value edge
    }
}
static int spaced(const ht_font_t *f, const char *t)
{
    int w = 0;
    for (const char *c = t; *c; c++) { char one[2] = {*c, 0}; w += ht_measure(f, one) + 2; }
    return w - 2;   // as focus_header draws it: a run a letter, 2 px between
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *pairs[][2] = {''' + ', '.join('{"%s", "%s"}' % p for p in pairs) + r'''};
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) row(pairs[i][0], pairs[i][1]);
    printf("prefs_row: no label meets its value in the terminal skin; the tightest gap is %d px\n", worst);
    // the heading: width from the Inter 20 metrics, the chord of the round glass at the title's row (TITLE_Y 62), the "<" control at x 87..135
    const int title_y = 62, back_right = 87 + 48;
    const double radius = HT_WIDTH / 2.0, dy = radius - title_y, chord = 2 * sqrt(radius * radius - dy * dy);
    const ht_font_t *small = &ht_lv_inter_20.base;
    const char *heads[] = {"SETTINGS", "ALL AGENTS"};
    for (int i = 0; i < 2; i++) {
        const int w = spaced(small, heads[i]), left = (HT_WIDTH - w) / 2, margin = (int)((chord - w) / 2);
        printf("heading %s: %d px wide, from x %d; glass chord %.0f px at y %d; margin %d px each side; clear of '<' by %d px\n", heads[i], w, left, chord, title_y, margin, left - back_right);
        assert(margin >= 24 && left - back_right >= 16);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-prefs-row-') as path:
    out = Path(path)
    (out / 'rows.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g', san, '-I', str(habitat), str(out / 'rows.c'),
                    str(habitat / 'terminal.c'), str(habitat / 'fonts.c'), str(habitat / 'focus_faces.c'), '-o', str(out / 'rows'), '-lm'], check=True)
    run = subprocess.run([str(out / 'rows')], capture_output=True, text=True)
    print(run.stdout, end='')
    assert run.returncode == 0, run.stdout + run.stderr

run_sh = text(here / 'run.sh')
assert 'test_v11_review_ui.py' in run_sh, 'run.sh runs this test'
print('v1.1 review fixes (ui_habitat.c): boot_voice carry, running_ask result, prefs_row layout, bright_drag_end, Swarm leave, label and moon, ALL AGENTS heading PASS')
