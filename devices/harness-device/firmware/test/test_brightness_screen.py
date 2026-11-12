"""The on-dial Settings screen and its Brightness slider (ui_habitat.c, brightness_arc.c).

Two kinds of check, because ui_habitat.c does not compile on a host:

  * BEHAVIOUR. The production touch helpers (bright_set, bright_drag_end, bright_touch, and ui_set_brightness under them) are cut out of the
    source and compiled against the real brightness_arc.c and small stubs. A scripted finger drives them: a drag that starts in the arc's band is
    the slider's until the finger lifts (even when it wanders out of the band), a contact that starts anywhere else is never taken, the level
    moves live and stays in 8..100, flash is asked for ONCE on lift (and not at all when nothing moved), and a cancelled contact saves too.
  * WIRING. The rest is read from the source with comments removed: the views and actions exist, the Settings row opens the Settings screen, the
    Brightness row opens the Brightness screen, the arc's touch is routed before every gesture, the old comment says what is true now, the
    screens are in the gallery, and the new file is built and tested.
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


def function(name, src=raw):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\n\{.*?^\}', src, re.M | re.S)
    assert match, f'no definition of {name}'
    return match.group(0)


# ── behaviour ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "brightness_arc.h"
#include "prefs_logic.h"
enum { HOME, BRIGHT, PREFS };
enum { A_NONE, A_SETTINGS_SAVE, A_PREFS_SAVE };
typedef struct { int kind; } action_t;
enum { UI_SETTING_BRIGHTNESS = 1u << 0, UI_SETTING_MUTED = 1u << 1 };
typedef struct { uint8_t brightness; bool muted; } ui_settings_t;
static struct { bool bright_drag, touch_down, touch_cancelled, muted, prefs_dirty, prefs_inflight; uint32_t prefs_rev; uint8_t prefs_fails; int bright_from, brightness, pressed, view; } s;
static struct { ui_settings_t values; uint32_t fields; } settings_pending;
static ht_prefs_t prefs;
#define pdPASS 1
typedef void *QueueHandle_t;
static QueueHandle_t actions = (void *)1;
static unsigned spaces = 8;
static int queued, last_kind, panel_writes, locks, saves, prefs_saves;
static uint8_t panel;
static unsigned uxQueueSpacesAvailable(QueueHandle_t q) { (void)q; return spaces; }
static int xQueueSend(QueueHandle_t q, const void *item, int wait) { (void)q; (void)wait; queued++; last_kind = ((const action_t *)item)->kind; saves += last_kind == A_SETTINGS_SAVE; prefs_saves += last_kind == A_PREFS_SAVE; return 1; }
static void display_lock(void) { locks++; }
static void display_unlock(void) { locks--; }
static void change(void) {}
static void display_set_brightness(uint8_t value) { panel = value; panel_writes++; }
'''
for name in ('ui_set_brightness', 'settings_save_queue', 'prefs_save_queue', 'bright_set', 'bright_drag_end', 'bright_touch'):
    harness += function(name) + '\n'
harness += r'''
static int level_of_byte(int byte) { return (byte * 100 + 127) / 255; }
// One finger: down at (x0, y0), then each (x, y) in turn, then up. Returns how many samples the slider took, and whether it took the first.
static int finger(const int (*path)[2], int n, bool *took_first)
{
    int took = 0;
    for (int i = 0; i < n; i++) {
        bool taken = bright_touch(true, path[i][0], path[i][1]);
        if (i == 0) *took_first = taken;
        took += taken;
        s.touch_down = true;                     // what habitat_touch does after a sample the slider took
    }
    bool taken = bright_touch(false, path[n - 1][0], path[n - 1][1]);
    took += taken;
    s.touch_down = false;
    return took;
}
int main(void)
{
    bool first;
    s.view = BRIGHT; s.brightness = 60; s.pressed = 3;

    // A drag that starts on the arc (right edge, 3 o'clock, y 233) and goes up: live level on every step, one save on lift.
    const int up[][2] = {{443, 233}, {441, 200}, {437, 150}, {420, 100}, {395, 40}};
    int took = finger(up, 5, &first);
    assert(first && took == 6 && "every sample of the drag, and the lift, belongs to the slider");
    assert(s.brightness == 100 && panel == 255 && s.pressed == -1);
    assert(panel_writes >= 3 && "the glass follows the finger, not just the lift");
    assert(queued == 1 && last_kind == A_SETTINGS_SAVE && "flash once, on lift");
    assert(settings_pending.fields == UI_SETTING_BRIGHTNESS && settings_pending.values.brightness == 100);
    assert(!s.bright_drag && locks == 0);

    // Dragging down: dimmer, and never below the floor of 8 percent, even with the finger far below the arc.
    queued = 0; settings_pending.fields = 0; panel_writes = 0;
    const int down[][2] = {{437, 150}, {441, 250}, {440, 350}, {400, 440}, {350, 465}};
    took = finger(down, 5, &first);
    assert(first && took == 6 && s.brightness == HT_BRIGHT_MIN && panel == (HT_BRIGHT_MIN * 255 + 50) / 100);
    assert(queued == 1 && settings_pending.values.brightness == HT_BRIGHT_MIN);

    // The level read back from the panel byte is the percent that was set: no drift in the save/load path.
    for (int p = HT_BRIGHT_MIN; p <= HT_BRIGHT_MAX; p++) {
        ui_set_brightness((uint8_t)((p * 255 + 50) / 100));
        assert(s.brightness == p);
        assert(level_of_byte((p * 255 + 50) / 100) == p);
    }

    // A drag that leaves the band keeps the slider's contact (it must not turn into a swipe or a scroll half way), and a finger far left of the
    // glass still sets the level by its height.
    s.brightness = 60; queued = 0; settings_pending.fields = 0;
    const int wander[][2] = {{443, 233}, {300, 200}, {150, 120}, {60, 60}, {30, 300}};
    took = finger(wander, 5, &first);
    assert(first && took == 6 && queued == 1 && "the contact is the slider's from first to last sample");

    // Touch and lift without moving off the level the contact began on: nothing to write. (443, 233 is 60 percent's height or close: set it first.)
    s.brightness = 60;
    int x, y;
    ht_bright_knob(60, &x, &y);
    queued = 0; settings_pending.fields = 0;
    const int still[][2] = {{x, y}, {x, y}};
    took = finger(still, 2, &first);
    assert(first && took == 3 && s.brightness == 60 && queued == 0 && settings_pending.fields == 0 && "no change, no write");

    // A contact that starts anywhere else is NEVER taken, not even after it crosses the band: back, the list, the middle, the left edge.
    const struct { int x, y; } elsewhere[] = {{110, 85}, {233, 233}, {60, 233}, {150, 440}, {233, 30}, {300, 233}};
    for (unsigned i = 0; i < sizeof elsewhere / sizeof elsewhere[0]; i++) {
        s.brightness = 60; queued = 0;
        const int path[][2] = {{elsewhere[i].x, elsewhere[i].y}, {420, 200}, {440, 150}, {440, 100}};
        took = finger(path, 4, &first);
        assert(!first && took == 0 && s.brightness == 60 && queued == 0 && "a contact that did not start on the arc keeps its normal meaning");
    }

    // Other screens never take a touch, even on the arc's band.
    s.view = PREFS; s.brightness = 60;
    const int on_arc[][2] = {{443, 233}, {441, 200}};
    took = finger(on_arc, 2, &first);
    assert(!first && took == 0 && s.brightness == 60);
    s.view = HOME;
    took = finger(on_arc, 2, &first);
    assert(!first && took == 0);
    s.view = BRIGHT;

    // A finger that was already down when the screen opened (touch_down) does not become a drag by sliding onto the arc.
    s.touch_down = true;
    assert(!bright_touch(true, 443, 233));
    s.touch_down = false;

    // A contact that is cancelled mid-drag (the glass lost the finger, a message moved the view) saves once, and later samples are not the slider's.
    s.brightness = 60; queued = 0; settings_pending.fields = 0;
    assert(bright_touch(true, 443, 233) && bright_touch(true, 437, 150));
    s.touch_down = true;
    bright_drag_end();                    // what input_cancel does
    assert(queued == 1 && s.brightness > 60 && !s.bright_drag);
    assert(!bright_touch(true, 437, 100) && !bright_touch(false, 437, 100) && queued == 1);
    bright_drag_end();
    assert(queued == 1 && "ending a drag twice saves once");
    s.touch_down = false;

    // A full action queue is not an error: the field stays pending, nothing blocks.
    s.brightness = 60; queued = 0; spaces = 1;
    assert(bright_touch(true, 443, 233) && bright_touch(true, 437, 150) && bright_touch(false, 437, 150));
    assert(queued == 0 && settings_pending.fields == UI_SETTING_BRIGHTNESS && "kept for the next save, the last queue slot left alone");
    // NIGHT MODE. Moving the arc while it is on ends it: the level the finger chose stays, the sound goes back to what it was, and both the level
    // and the prefs are asked to be written (two writes on lift: A_SETTINGS_SAVE and A_PREFS_SAVE).
    spaces = 8; queued = saves = prefs_saves = 0; settings_pending.fields = 0; s.prefs_dirty = false;
    s.brightness = 8; s.muted = true;
    prefs.night = true; prefs.night_bright = 70; prefs.night_muted = false;
    assert(bright_touch(true, 443, 233) && bright_touch(true, 437, 150) && bright_touch(false, 437, 150));
    assert(!prefs.night && s.brightness == ht_bright_percent_at(150) && !s.muted && "the user's level stays; sound back");
    assert(saves == 1 && prefs_saves == 1 && s.prefs_dirty && s.prefs_inflight && s.prefs_rev == 1 && "dirty until the worker confirms the write");
    // A second ask while one is in flight queues nothing more; a refused ask (full queue) stays dirty for the tick to ask again.
    prefs_save_queue();
    assert(prefs_saves == 1);
    s.prefs_inflight = false;                // what the worker does after a FAILED write: dirty stays, the tick asks again
    spaces = 1; prefs_save_queue();
    assert(prefs_saves == 1 && s.prefs_dirty && !s.prefs_inflight && "a full queue is not a success");
    spaces = 8; prefs_save_queue();
    assert(prefs_saves == 2 && s.prefs_inflight && "the retry");
    s.prefs_inflight = false; s.prefs_dirty = false;   // the worker's success
    assert((settings_pending.fields & (UI_SETTING_BRIGHTNESS | UI_SETTING_MUTED)) == (UI_SETTING_BRIGHTNESS | UI_SETTING_MUTED) && !settings_pending.values.muted);
    // With Night mode off, a drag asks for no prefs write.
    queued = saves = prefs_saves = 0; s.brightness = 60; s.prefs_dirty = false; s.prefs_inflight = false;
    assert(bright_touch(true, 443, 233) && bright_touch(true, 437, 150) && bright_touch(false, 437, 150));
    assert(saves == 1 && prefs_saves == 0);
    puts("brightness touch: live level, one save on lift, floor 8, band-only start, whole contact, cancel, full queue PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-bright-screen-') as directory:
    out = Path(directory)
    (out / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), '-I', str(habitat),
                    str(out / 'test.c'), str(habitat / 'brightness_arc.c'), str(habitat / 'prefs_logic.c'), str(habitat / 'terminal.c'), str(habitat / 'fonts.c'),
                    '-o', str(out / 'test'), '-lm'], check=True)
    subprocess.run([str(out / 'test')], check=True)

# ── wiring ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
enum_view = ui[ui.index('typedef enum {\n    HOME'):ui.index('} view_t;')]
for v in ('PREFS', 'BRIGHT', 'ABOUT'):
    assert re.search(r'\b' + v + r'\b', enum_view), v
enum_action = ui[ui.index('typedef enum {\n    A_NONE'):ui.index('} action_kind_t;')]
for a in ('A_PREFS', 'A_BRIGHT', 'A_SOUND', 'A_PLINK', 'A_PLINK_SYNC', 'A_PLINK_CYCLE', 'A_SCREEN_OFF', 'A_NIGHT', 'A_SAVER', 'A_ABOUT',
          'A_RESTART', 'A_RESTART_GO', 'A_PREFS_SAVE'):
    assert re.search(r'\b' + a + r'\b', enum_action), a
assert 'case PREFS: return "prefs";' in ui and 'case BRIGHT: return "brightness";' in ui and 'case ABOUT: return "about";' in ui, 'habitat_view_name'
dispatch = function('dispatch', ui)
for a in ('A_PLINK_SYNC', 'A_PLINK_CYCLE', 'A_RESTART_GO', 'A_PREFS_SAVE'):
    assert re.search(r'case ' + a + r':\n(?:\s*case A_\w+:\n)*\s*break;', dispatch), a + ' is the worker\'s alone'

# The Settings row, in the list of the controls, always enabled (it is local, like Inbox and Nap). The Phone row MOVED out of that list.
items = re.search(r'menu_items\[\] = \{.*?\n\};', ui, re.S).group(0)   # the table (dial-reach T10: it is file-scope, with a group field)
assert '{"Settings", A_PREFS, 0, MENU_GROUP_DIAL}' in items, 'the row'
assert 'A_PHONE' not in items and '"Phone"' not in items, 'Phone is no longer a row of the controls list'
assert re.search(r'action == A_PHONE \|\| action == A_PREFS \|\|', function('render_settings', ui)), 'enabled without a daemon'

# Its screen: the rows come from one table, in the list geometry; the back control and the title.
prefs = function('render_prefs', ui)
assert 'focus_header(f, "SETTINGS")' in prefs and re.search(r'control\(f, \d+, \d+, \d+, "<", A_SETTINGS, 1, true\)', prefs), 'title, back to the list'
assert 'prefs_item(' in prefs and 'LIST_HIT_X' in prefs and 'TAB_ROW_HEIGHT' in prefs, 'rows come from a table, in the list geometry'
assert 'Tap again to restart' in prefs, 'Restart dial shows its second tap'
table = function('prefs_item', ui)
rows = re.findall(r'\{"([A-Za-z ]+)", (A_[A-Z_]+)\}', table)
assert rows == [('Brightness', 'A_BRIGHT'), ('Sound', 'A_SOUND'), ('Unplugged link', 'A_PLINK'), ('Screen off', 'A_SCREEN_OFF'), ('Night mode', 'A_NIGHT'),
                ('Battery saver', 'A_SAVER'), ('Phone', 'A_PHONE'), ('About', 'A_ABOUT'), ('Restart dial', 'A_RESTART')], rows
assert re.search(r'#ifdef DEVICE_BLE_LINK\n\s*\{"Phone", A_PHONE\},[^\n]*\n#endif', raw[raw.index('static const char *prefs_item'):]), 'Phone only with the phone link'
assert 'prefs_link_shown()' in table, 'Unplugged link hides when fewer than two modes are on offer'
assert 'ht_link_options(prefs_link.wifi, prefs_link.phone, PREFS_BLE_IMAGE, prefs_link.bonded) >= 2' in function('prefs_link_shown', ui)
assert re.search(r'items\[i\]\.action == A_SCREEN_OFF && !PREFS_SLEEP_IMAGE\) continue;', table), 'no Screen off row in an image without the battery policy'
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n#define PREFS_SLEEP_IMAGE true\n#else\n#define PREFS_SLEEP_IMAGE false', raw)
assert re.search(r'display_set_brightness_cap\(\(uint8_t\)ht_panel_cap\(&prefs\)\);\n#ifdef DEVICE_BATTERY_SLEEP\n\s*power_sleep_set_screen_off_ms\(ht_screen_off_ms\(&prefs\)\);\n\s*power_sleep_set_wifi_park_min\(\(unsigned\)ht_park_minutes\(&prefs\)\);\n#endif', function('prefs_apply_power', raw)), 'without the battery policy Battery saver is the panel cap alone'
bright = function('render_bright', ui)
assert 'focus_header(f, "BRIGHTNESS")' in bright and re.search(r'control\(f, \d+, \d+, \d+, "<", A_PREFS, 1, true\)', bright), 'title, back to Settings'
assert 'ht_bright_arc(' in bright and 'bright_level()' in bright
about = function('render_about', ui)
assert re.search(r'control\(f, \d+, \d+, \d+, "<", A_PREFS, 1, true\)', about) and 'focus_header(f, "ABOUT")' in about
for secret in ('ssid', 'pass', 'key', 'config_net', 'host', 'token'):
    assert secret not in about.lower(), f'About must not touch {secret}'
for want in ('esp_app_get_description()->version', 'battery_pct()', 'link_icon_kind()', 'about_uptime('):
    assert want in about, want

# Dispatch of each row.
assert re.search(r'case A_PREFS: \{\s*int keep = 0;\s*if \(s\.view == SETTINGS\) s\.list_offset = s\.offset;\s*else if \(a\.value\) keep = s\.prefs_offset;\s*view\(PREFS\);\s*s\.offset = keep;\s*queue\(\(action_t\)\{\.kind = A_PLINK_SYNC\}\);', ui), 'A_PREFS'
assert re.search(r'case A_BRIGHT:\s*s\.prefs_offset = s\.offset;\s*view\(BRIGHT\);\s*break;', ui), 'A_BRIGHT'
assert re.search(r'case A_ABOUT:\s*s\.prefs_offset = s\.offset;\s*s\.about_at = ms\(\) \+ 10000;\s*view\(ABOUT\);', ui), 'A_ABOUT'
assert re.search(r'case A_SETTINGS: \{\s*const int keep = a\.value \? s\.list_offset : 0;[^\n]*\n\s*menu_line_t lines\[MENU_MAX_LINES\];\s*const int n = settings_lines\(lines\);\s*view\(SETTINGS\);\s*s\.offset = menu_clamp\(lines, n, TAB_ROWS, keep\);', ui), 'back lands on the row it left, kept valid by the model'
sound = dispatch[dispatch.index('case A_SOUND:'):dispatch.index('case A_PLINK:')]
assert 's.muted = !s.muted;' in sound and 'UI_SETTING_MUTED' in sound and 'settings_save_queue()' in sound and 'config_' not in sound, 'Sound: the muted flag through A_SETTINGS_SAVE'
screen_off = dispatch[dispatch.index('case A_SCREEN_OFF:'):dispatch.index('case A_NIGHT:')]
assert 'ht_timer_next(prefs.timer)' in screen_off and 'prefs_apply_power()' in screen_off and 'prefs.saver' in screen_off.split('prefs_apply_power')[0], 'Screen off cycles, and Battery saver holds it'
night = dispatch[dispatch.index('case A_NIGHT: {'):dispatch.index('case A_SAVER:')]
assert 'ht_night_toggle(&prefs, &bright, &muted);' in night and 'UI_SETTING_BRIGHTNESS | UI_SETTING_MUTED' in night and 'prefs_changed()' in night
assert re.search(r'if \(prefs\.night\) \{\s*prefs_changed\(\);\s*settings_save_queue\(\);\s*\} else \{\s*settings_save_queue\(\);\s*prefs_changed\(\);\s*\}', night), 'ON: flag and saved pair first; OFF: restored levels first'
saveq = function('prefs_save_queue', ui)
assert 's.prefs_dirty = false' not in saveq and 's.prefs_inflight = true' in saveq, 'queueing never clears dirty: only a confirmed write does'
pw = function('worker', ui)
pw = pw[pw.index('case A_PREFS_SAVE:'):]
pw = pw[:pw.index('break;')]
assert re.search(r'const bool ok = config_save_prefs\(&c\);.*if \(ok\) \{\s*if \(s\.prefs_rev == rev\) s\.prefs_dirty = false;', pw, re.S), 'dirty clears after a success, for the revision written'
assert '++s.prefs_fails == 5' in pw and 's.prefs_inflight = false;' in pw
assert 'if (s.prefs_dirty && !s.prefs_inflight && s.prefs_fails < 5) prefs_save_queue();' in function('habitat_tick', ui), 'the tick asks again after a failure'
saver = dispatch[dispatch.index('case A_SAVER:'):dispatch.index('case A_RESTART:')]
assert 'prefs.saver = !prefs.saver;' in saver and 'prefs_apply_power()' in saver and 'prefs_changed()' in saver
power = function('prefs_apply_power', ui)
for call in ('display_set_brightness_cap((uint8_t)ht_panel_cap(&prefs))', 'power_sleep_set_screen_off_ms(ht_screen_off_ms(&prefs))', 'power_sleep_set_wifi_park_min((unsigned)ht_park_minutes(&prefs))'):
    assert call in power, call
assert 'prefs_apply_power();' in function('ui_init', ui) and 'config_load_prefs(&c)' in function('ui_init', ui), 'applied at boot, before the first frame'
# Restart: the first tap arms, the second queues what is pending, then the restart; the worker restarts after the writes and says why.
restart = dispatch[dispatch.index('case A_RESTART:'):dispatch.index('case A_RESTART:') + 1400]
assert 's.restart_until = ms() + 3000;' in restart
second = restart[:restart.index('} else {')]
assert second.index('A_SETTINGS_SAVE') < second.index('A_PREFS_SAVE') < second.index('A_RESTART_GO'), 'pending writes are queued before the restart'
worker = function('worker', ui)
go = worker[worker.index('case A_RESTART_GO:'):]
go = go[:go.index('break;')]
assert 'mode_glue_user_restart("restart from Settings")' in go and 'ESP_LOGW("habitat", "restart: asked from Settings")' in go, 'a clear reason'
assert 'esp_restart' not in worker, 'the worker never restarts by itself: the store gate and the busy gate decide'
assert 'prefs_note("Busy, try again")' in go, 'a refusal is said'
assert 'case A_PREFS_SAVE:' in worker and 'config_save_prefs(&c)' in worker
# Unplugged link: the stored wl_mode through the same call the computer's command uses; the mode switch is told; a phone with no bond gets a note.
link = function('prefs_link_work', ui)
for want in ('ht_link_next(info.mode, info.saved, info.ble, PREFS_BLE_IMAGE, bonded)', 'config_net_set_mode(next)', 'mode_glue_settings_changed()', 'ble_link_bond_count() > 0', 'pair it in Settings', 'prefs_link.bonded = bonded;'):
    assert want in link, want
assert 'esp_restart' not in link, 'the row itself never reboots: the mode switch decides (USB with a cable stays USB)'

# The touch: routed before the gesture code, which is what keeps a swipe or a scroll from firing during or because of the drag.
touch = function('habitat_touch', ui)
route = touch.index('bright_touch(down, x, y)')
assert route < touch.index('ht_gesture_'), 'before every gesture call'
assert route < touch.index('ht_scroll_'), 'before every scroll call'
assert route < touch.index('tabs_move('), 'before the list scroll'
assert re.search(r'if \(bright_touch\(down, x, y\)\) \{\s*s\.touch_down = down;\s*s\.last_x = x;\s*s\.last_y = y;\s*return;\s*\}', touch), 'the sample is consumed'
assert 'bright_drag_end();' in function('input_cancel', ui), 'a cancelled contact saves what the finger set'
assert re.search(r'static void bright_drag_end\(void\);', ui), 'declared before input_cancel'
assert 'ui_set_brightness(' in function('bright_set', ui) and 'config_save_brightness' not in function('bright_set', ui), 'live, no flash on a move'
assert 'config_save_brightness' not in function('bright_drag_end', ui) and 'settings_save_queue()' in function('bright_drag_end', ui), 'flash goes through the worker'
worker_save = ui[ui.index('case A_SETTINGS_SAVE: {'):]
worker_save = worker_save[:worker_save.index('ui_settings_changed();')]
assert 'config_save_brightness((uint8_t)((want.brightness * 255 + 50) / 100));' in worker_save, 'the worker writes the byte and then tells the desktop'
assert 'audio_notify_set_muted(want.muted)' in worker_save, 'Sound goes through the same worker write'

# The lists scroll: the Settings screen is in every place the controls list is.
assert 's.view == PREFS ? prefs_count()' in ui and ui.count('s.view == PREFS') >= 5, 'scroll limits and drag handling'
# The screens that sit beside it: the link icon gives way to the arc, a drag holds the automatic mode switch.
assert re.search(r'static void render_link_icon\(ht_scene_t \*f\)\n\{\n    if \(s\.view == BRIGHT\) return;', ui), 'the link icon is not drawn where the arc is'
assert re.search(r'case FORM: case SELECTION: case STOP: case FORGET: case BRIGHT:', function('ui_mode_switch_busy', ui)), 'no mode switch mid-drag'
# The restart arming, the note and the About redraw all expire on the tick, and the wake deadline list knows them.
tick = function('habitat_tick', ui)
assert 's.restart_until' in tick and 's.note_until' in tick and 's.view == ABOUT' in tick
assert 's.restart_until, s.note_until};' in function('habitat_next_wake_ms', ui)
assert 'if (v != PREFS) { s.restart_until = 0;' in function('view', ui), 'leaving the screen disarms Restart dial'

# The comment that said Brightness and Sound were gone from the dial says what is true now.
old = raw[raw.index(' * THE COMPANION SCREEN IS GONE'):]
old = old[:old.index('*/')]
assert 'Brightness' in old and 'Settings' in old and 'came back' in old, 'the comment says Brightness is back, under Settings'
assert 'and so are Brightness and Sound from the list' not in old and 'Only their screens went' not in old, 'the false claims are gone'

# Power: the radio park after 2 minutes cannot touch what the mode switch does about a parked radio (H1, mode_glue.c): that hold reads the radio's STATE.
glue = text(fw / 'main/mode_glue.c')
assert 'w->link_hold = net_link_radio_waking();' in glue, 'the H1 line is as it was'
net = text(fw / 'main/net_link.c')
waking = net[net.index('bool net_link_radio_waking(void)'):]
waking = waking[:waking.index('\n}\n')]
assert 'park' not in waking.lower() and 'min' not in waking.lower().replace('timer', '') and 'power_sleep' not in waking, 'the hold reads state and a 30 s wake window, not the park time'
sleep = text(fw / 'main/power_sleep.c')
assert 'pp_wifi_off_wanted(&s_off, in.now_ms, (uint32_t)park_min * 60u * 1000u)' in sleep and 'S2_WIFI_OFF_MIN' in sleep, 'one place uses the park time, default S2_WIFI_OFF_MIN'
assert re.search(r'power_sleep_screen_level\(uint32_t idle_ms\)\n\{[^}]*pp_screen_level_cfg\(', sleep, re.S), 'the screen policy takes the user time'
# Brightness cap in the renderer: before the dim step, so dim is a quarter of what the panel shows.
disp = text(habitat / 'display_habitat.c')
assert disp.index('const unsigned cap = atomic_load(&brightness_cap);') < disp.index('brightness = pp_dimmed_percent(brightness);'), 'cap, then dim'

# The gallery draws every new view, the same way as the others.
inc = text(habitat / 'ui_gallery.inc')
items_h = text(habitat / 'ui_gallery_items.h')
for sym, view_name, source in (('prefs', 'prefs', 'render_prefs'), ('prefs_2', 'prefs', 'render_prefs'), ('bright_mid', 'brightness', 'render_bright'),
                               ('bright_low', 'brightness', 'render_bright'), ('bright_full', 'brightness', 'render_bright'), ('about', 'about', 'render_about')):
    assert re.search(r'SETUP\(' + sym + r'\)', inc), sym
    assert re.search(r'X\(' + sym + r',\s+"' + sym + r'",\s+0, 2000, "' + view_name + r'",\s+"' + source + r'"', items_h), sym

# Built, and tested.
cmake = text(fw / 'main/CMakeLists.txt')
assert '"ui/habitat/brightness_arc.c"' in cmake and '"ui/habitat/prefs_logic.c"' in cmake, 'both files are in BOARD_SRCS'
run = text(here / 'run.sh')
assert 'test_brightness_arc' in run and 'test_brightness_screen.py' in run and 'test_prefs_logic' in run, 'run.sh runs all three'

# Restart dial goes through the mode switch's gate (store-gate freeze, busy look) and not esp_restart() on its own.
glue_src = code(fw / 'main/mode_glue.c')
ug = glue_src[glue_src.index('static bool gate_hard_busy'):glue_src.index('int mode_glue_user_restart')]
assert '(mode_switch_busy() & MS_BUSY_HARD) != 0' in ug and 'USER_GATE = {NULL, gate_frozen, gate_freeze, gate_thaw, gate_hard_busy, gate_restart}' in ug, 'the same freeze and restart, the hard rows only'
ur = glue_src[glue_src.index('int mode_glue_user_restart'):]
ur = ur[:ur.index(chr(10) + '}' + chr(10))]
assert 'mg_restart_try(&USER_GATE)' in ur and ur.index('last_words_note_reason(reason)') < ur.index('mg_restart_try') < ur.index('last_words_note_reason("")'), 'reason noted, gate tried, reason blanked after a refusal'
assert 'int mode_glue_user_restart(const char *reason);' in text(fw / 'main/mode_glue.h')
# The failed cable start frees the PSRAM stack of the session task (the start is retried, so a plain vTaskDelete would leak 4 KB each time).
cc_src = code(fw / 'main/cable_client.c')
fail = cc_src[cc_src.index('if (!cable_link_start_with(xport, on_frame, session_tick, NULL)) {'):]
fail = fail[:fail.index('goto failed;')]
assert 'task_delete_psram(hello_task);' in fail and '#if TASK_STACK_PSRAM_FLASH_OPS' in fail, 'task_delete_psram on a PSRAM-stack task'
tu = code(fw / 'main/task_util.c')
assert 'xTaskGetStaticBuffers(task, &stack, &tcb) == pdTRUE) vTaskDeleteWithCaps(task);' in tu and 'else vTaskDelete(task);' in tu, 'caps tasks free their PSRAM stack, the internal fallback gets the plain delete'
print('settings screen wiring: views, rows, dispatch, worker, touch routing before gestures, restart, link, power, comment, gallery, build PASS')
