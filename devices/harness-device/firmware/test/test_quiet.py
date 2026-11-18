"""The Quiet row and the moon mark (dial-reach T11; plan amendment B4; user decisions 2026-11-12).

The menu's DIAL group has a row that rests the dial for 15 minutes (internally s.nap and A_NAP: s.quiet is ANOTHER setting, the desktop's "creature animations
off"). Off, it reads "Quiet 15 min"; on, "Quiet NN min left" with NN the whole minutes left, rounded up, 1..15. The dial has no wall clock (nothing in
firmware/main reads one), so the row cannot say "until HH:MM". While the menu is up the row repaints once a minute, not at the frame rate. A moon mark
shows on the home face while Quiet is on.

  * source rules on comment-stripped ui_habitat.c: the words, no "Nap" or "Wake", the toggle, the tick and wake-deadline wiring, the moon's guard;
  * a compiled harness of the production nap_tick and nap_next_minute_ms: 600 ticks inside one minute repaint nothing, a minute change repaints once,
    the end of the nap clears it once, nothing repaints off the menu;
  * a compiled harness of the production render_moon against the REAL character renderer: for every character, engine and name, the asleep face holds no run
    that the moon's two discs touch (the mark is not a hit and covers nothing);
  * the longest row text fits the row in both skins.
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


def function(name, src=ui):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, 'no definition of ' + name
    return match.group(0)


def define(name):
    match = re.search(r'#define\s+' + name + r'\s+(\d+)', ui)
    assert match, name
    return int(match.group(1))


# --- source rules -------------------------------------------------------------------------------------------------------------------------------
render = function('render_settings')
assert '"Quiet %d min left"' in render, 'the row says how many minutes are left'
assert 'menu_quiet_minutes(ms(), s.nap_until)' in render, 'the count comes from the model'
assert re.search(r'menu_quiet_minutes\(ms\(\), s\.nap_until\);\s*if \(minutes >= 1\)|if \(minutes >= 1\)', render) or 'minutes > 0' in render, 'the count text is used only for 1 or more minutes'
table = re.search(r'menu_items\[\] = \{.*?\n\};', ui, re.S).group(0)
assert '"Quiet 15 min"' in table, 'the row when Quiet is off'
for old in ('"Nap"', '"Wake"'):
    assert old not in render and old not in table, old + ' is gone from the menu'
assert '"Wake"' not in ui, 'no "Wake" anywhere on screen'
nap = ui[ui.index('case A_NAP:'):]
nap = nap[:nap.index('break;')]
assert 's.nap = !s.nap;' in nap and 's.nap_until = ms() + 15 * 60 * 1000;' in nap, 'the toggle and the 15 minutes'
assert 's.nap_shown' in nap, 'the minute last drawn starts at 15'
assert 'NOT s.quiet' in raw[raw.index('case A_NAP:'):raw.index('case A_NAP:') + 700], 'a comment at A_NAP says s.quiet is another setting'
assert 'nap_tick(now);' in function('habitat_tick'), 'the tick ends the nap and repaints once a minute'
assert 'nap_next_minute_ms(now)' in function('habitat_next_wake_ms'), 'the wake list knows the next minute boundary'
assert re.search(r'if \(s\.nap\)\s*render_moon\(f\);', function('render_home')), 'the moon is drawn only while Quiet is on'
assert 'hits[' not in function('render_moon') and 'hit_count' not in function('render_moon'), 'the moon is not a hit'
assert 'ht_box(' in function('render_moon') and function('render_moon').count('ht_box(') == 2, 'two discs: the moon and the one that cuts it'
assert 'int nap_shown' in ui or 'nap_shown;' in ui, 'one int in the state struct'
# a tap on the face ends the rest on both skins (the comment at A_NAP says so): the Focus face's consumed tap and the sibling tap on a surface clear s.nap
# and repaint, and neither dispatches A_PET (no boop) or starts voice
release = function('habitat_touch')
focus_tap = release[release.index('character.id == HT_CHARACTER_FOCUS && pressed_action.kind == A_PET'):]
focus_tap = focus_tap[:focus_tap.index('} else if')]
assert re.search(r'if \(s\.nap\)\s*\{\s*s\.nap = false;\s*change\(\);\s*\}', focus_tap), 'a Focus face tap ends Quiet and repaints'
assert 'dispatch(' not in focus_tap, 'the consumed Focus face tap dispatches nothing'
surface_tap = release[release.index('result == HT_TOUCH_TAP && pressed_action.kind == A_PET &&'):]
surface_tap = surface_tap[:surface_tap.index('} else if (result == HT_TOUCH_HOLD)')]
assert re.search(r'if \(surface && s\.nap\)\s*\{\s*s\.nap = false;\s*change\(\);\s*\}', surface_tap), 'a face tap on a surface ends Quiet on every skin'
assert 'dispatch(pressed_action)' not in surface_tap, 'the surface face tap dispatches no A_PET'

SET_TEXT_W, LIST_HIT_W = define('SET_TEXT_W'), define('LIST_HIT_W')

# --- the tick, compiled ----------------------------------------------------------------------------------------------------------------------------
tick = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "menu_list.h"
enum { HOME, SETTINGS };
static struct { bool nap; int view; uint32_t nap_until; int nap_shown; } s;
static int changes;
static void change(void) { changes++; }
''' + function('nap_tick') + '\n' + function('nap_next_minute_ms') + r'''
int main(void)
{
    const uint32_t MIN = 60000u, t0 = 100000u;
    // the menu is up, Quiet was just turned on: 600 ticks (100 ms apart) inside the first minute repaint nothing
    s.nap = true; s.view = SETTINGS; s.nap_until = t0 + 15 * MIN; s.nap_shown = 15; changes = 0;
    for (int k = 0; k < 600; k++) nap_tick(t0 + 100u * (uint32_t)k);
    assert(changes == 0 && s.nap);
    // the minute turns: one repaint, not one per tick
    nap_tick(t0 + MIN); assert(changes == 1 && s.nap_shown == 14);
    for (int k = 1; k < 600; k++) nap_tick(t0 + MIN + 100u * (uint32_t)k);
    assert(changes == 1);
    // over ten minutes at 10 Hz: one repaint per minute change, 15 -> 5 is ten of them
    s.nap_shown = 15; changes = 0;
    for (int k = 0; k <= 6000; k++) nap_tick(t0 + 100u * (uint32_t)k);
    assert(changes == 10 && s.nap_shown == 5 && s.nap);
    // the row is not on screen: no repaint at all, and no wake
    s.view = HOME; s.nap_shown = 15; changes = 0;
    for (int k = 0; k <= 3000; k++) nap_tick(t0 + 100u * (uint32_t)k);
    assert(changes == 0 && nap_next_minute_ms(t0) == 0);
    // the end of the nap clears it once, in any view
    s.view = SETTINGS; s.nap = true; changes = 0;
    nap_tick(t0 + 15 * MIN - 1); assert(s.nap);
    nap_tick(t0 + 15 * MIN); assert(!s.nap && changes >= 1);
    int after = changes; nap_tick(t0 + 15 * MIN + 100); assert(changes == after);   // off: nothing more
    s.view = HOME; s.nap = true; s.nap_until = t0 + 15 * MIN; changes = 0;
    nap_tick(t0 + 16 * MIN); assert(!s.nap && changes == 1);
    // the wake deadline: the next minute boundary while the row is up and more than a minute is left
    s.nap = true; s.view = SETTINGS; s.nap_until = t0 + 15 * MIN;
    assert(nap_next_minute_ms(t0) == t0 + MIN);
    assert(nap_next_minute_ms(t0 + 1) == t0 + MIN);
    assert(nap_next_minute_ms(t0 + MIN) == t0 + 2 * MIN);
    assert(nap_next_minute_ms(t0 + 14 * MIN - 1) == t0 + 14 * MIN);
    assert(nap_next_minute_ms(t0 + 14 * MIN) == 0);   // the last minute: the nap's own end is the deadline
    assert(nap_next_minute_ms(t0 + 15 * MIN) == 0);
    s.nap = false; assert(nap_next_minute_ms(t0) == 0);
    // the 32-bit clock wraps
    s.nap = true; s.nap_until = 0xFFFFFF00u + 15 * MIN; s.nap_shown = 15; changes = 0;
    for (int k = 0; k <= 700; k++) nap_tick(0xFFFFFF00u + 100u * (uint32_t)k);
    assert(changes == 1 && s.nap_shown == 14);
    puts("quiet tick: no repaint inside a minute, one per minute change, none off the menu, the end clears it once, the wake deadline follows the minute, wrap PASS");
    return 0;
}
'''

# --- the moon and the row's width, compiled against the real renderer ------------------------------------------------------------------------------
moon = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "terminal.h"
#include "character.h"
#include "focus_faces.h"
#include "menu_list.h"
#define FG 0xffffu
#define BG 0x0000u
''' + function('render_moon') + r'''
static int overlap(ht_rect_t a, ht_rect_t b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
int main(void)
{
    const char *engines[] = {"claude", "codex", "muse", "cursor", "terminal", "gemini", "pi", ""};
    const char *names[] = {"Payments refactor", "A", "A very long agent name that wraps over the arc"};
    int faces = 0;
    static ht_scene_t scene;
    for (int id = 0; id < HT_CHARACTER_COUNT; id++) {
        ht_character_t c = {0};
        if (!ht_character_select(&c, (ht_character_id_t)id)) continue;
        for (unsigned e = 0; e < sizeof engines / sizeof *engines; e++)
        for (unsigned n = 0; n < sizeof names / sizeof *names; n++)
        for (int bell = 0; bell < 2; bell++)
        for (int status = 0; status < 2; status++) {
            ht_scene_clear(&scene, 0);
            ht_character_face_t f = {.recipient = names[n], .status = status ? "Try again" : "", .hint = "", .tab = "Main", .engine = engines[e], .activity = "",
                .mood = HT_CHARACTER_ASLEEP, .clock_ms = 0, .notice_ms = bell ? 1000 : 0, .notices = bell ? 2 : 0, .ink = 0xffff, .foreground = 0xffff,
                .dim = 0x8410, .primary_title = true, .roomy_reading = true};
            ht_character_face(&scene, &c, &f, 0xffff, NULL);
            const int before = scene.count;
            render_moon(&scene);
            assert(scene.count == before + 2);           // two discs, nothing else
            ht_rect_t d0 = ht_run_bounds(&scene.runs[before]), d1 = ht_run_bounds(&scene.runs[before + 1]);
            assert(d0.w > 0 && d1.w > 0);
            // the cut is a crescent: the second disc covers part of the first, not all of it, and sticks out
            assert(overlap(d0, d1) && d1.x > d0.x);
            for (int i = 0; i < before; i++) {
                ht_rect_t r = ht_run_bounds(&scene.runs[i]);
                if (r.w <= 0 || r.h <= 0) continue;
                assert(!overlap(d0, r) && !overlap(d1, r));
            }
            // inside the round glass: every corner of both discs is within the circle
            const ht_rect_t both[2] = {d0, d1};
            for (int k = 0; k < 2; k++) {
                const int xs[2] = {both[k].x, both[k].x + both[k].w}, ys[2] = {both[k].y, both[k].y + both[k].h};
                for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
                    int dx = xs[a] - 233, dy = ys[b] - 233;
                    assert(dx * dx + dy * dy <= 215 * 215);
                }
            }
            faces++;
        }
    }
    assert(faces > 1000);
    // the row: the longest text, "Quiet 15 min left" (17 characters), fits in both skins
    char row[32];
    for (int m = 1; m <= 15; m++) {
        snprintf(row, sizeof row, "Quiet %d min left", m);
        assert(strlen(row) <= 17);
        assert((int)strlen(row) * ht_mono_28.width <= SET_TEXT_W_VALUE);          // the terminal skin: fixed cells in the row's text width
        assert(ht_measure(&ht_lv_inter_28.base, row) <= LIST_HIT_W_VALUE);        // the Focus skin: Inter, centred in the row
    }
    assert((int)strlen("Quiet, 15 min left") * ht_mono_28.width > SET_TEXT_W_VALUE);   // why there is no comma: 18 characters do not fit
    printf("quiet moon: %d asleep faces, the two discs touch no other run and stay on the glass; the longest row text fits both skins PASS\n", faces);
    return 0;
}
'''.replace('SET_TEXT_W_VALUE', str(SET_TEXT_W)).replace('LIST_HIT_W_VALUE', str(LIST_HIT_W))

sources = [str(fw / 'main/pet_store.c'), str(fw / 'main/elapsed.c')] + [str(habitat / n) for n in (
    'character.c', 'illustrated.c', 'character_motion.c', 'character_layout.c', 'tux.c', 'focus.c', 'lvgl_fonts.c', 'lvgl_icons.c', 'focus_marks.c',
    'focus_faces.c', 'pets.c', 'octopus.c', 'octopus_font.c', 'ascii_clip.c', 'terminal.c', 'fonts.c')]
san = '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds')
with tempfile.TemporaryDirectory(prefix='harness-quiet-') as path:
    out = Path(path)
    (out / 'tick.c').write_text(tick)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', san, '-I', str(habitat), str(out / 'tick.c'), str(habitat / 'menu_list.c'),
                    '-o', str(out / 'tick')], check=True)
    subprocess.run([str(out / 'tick')], check=True)
    (out / 'moon.c').write_text(moon)
    # terminal.c wants the POSIX string functions (strnlen) the way the board's libc has them
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Wno-error', '-O1', '-g', san,
                    '-I', str(fw / 'main'), '-I', str(habitat), str(out / 'moon.c'), str(habitat / 'menu_list.c'), *sources, '-o', str(out / 'moon'), '-lm'], check=True)
    subprocess.run([str(out / 'moon')], check=True)
print('quiet: row words, minute count, once-a-minute repaint, wake deadline, moon mark off every face and off the hit list, row fits both skins PASS')
