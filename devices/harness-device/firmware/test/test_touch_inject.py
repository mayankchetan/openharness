"""Run the production touch task with an injected gesture standing in for the glass.

test_touch_driver.py proves the task against controller traces; this proves the debug hook added to it
(debug_cmd.h: dbg_inject_poll) feeds the SAME path a finger does — press count, wake swallow, activity
bump, habitat_touch() — and leaves a build without the hook, or a pass with nothing armed, untouched.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

p = Path(__file__).resolve().parent
source = (p / '../main/ui/habitat/touch_habitat.c').read_text()
body = re.search(r'static void task\(void \*arg\)\n\{.*?^\}', source, re.M | re.S).group(0)
assert 'HARNESS_DEBUG_COMMANDS' in body and 'dbg_inject_poll' in body, 'the hook is not in the task'
code = r'''
#define HARNESS_DEBUG_COMMANDS 1
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "terminal.h"
#include "debug_cmd.h"
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE 1
#define ESP_FAIL 2
#define TOUCH_CST9217 9217
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
static void *controller = (void *)1, *io = (void *)1;
static atomic_uint presses, failures, inferred, last_press;
static atomic_bool held, controller_ready;
static struct {int touch;} b = {9217};
static const void *board_(void) { return &b; }
#define board() ((const typeof(b) *)board_())
typedef struct {uint32_t ms; int rc;} pass_t;
static pass_t *passes; static int count, at, bumps, canceled, woke;
static bool asleep, open_ok;
static jmp_buf stop;
typedef struct {bool down; int x, y;} event_t;
static event_t events[400]; static int event_count;
static int64_t esp_timer_get_time(void) { return passes[at].ms * 1000ll; }
static bool open_touch(void) { if (open_ok) controller = (void *)1; return open_ok; }
static void ulTaskNotifyTake(int a, int t) { (void)a; (void)t; if (++at == count) longjmp(stop, 1); }
static int esp_lcd_touch_read_data(void *c) { (void)c; return passes[at].rc; }
// The glass itself is always idle: the only contact in these runs is the injected one.
static bool esp_lcd_touch_get_coordinates(void *c, uint16_t *x, uint16_t *y, uint16_t *s, uint8_t *n, int m)
{ (void)c; (void)s; (void)m; *x = 0; *y = 0; *n = 0; return false; }
static void esp_lcd_touch_del(void *c) { (void)c; }
static void esp_lcd_panel_io_del(void *c) { (void)c; }
static void display_lock(void) {}
static void display_unlock(void) {}
static bool display_is_asleep(void) { return asleep; }
static void display_wake(void) { asleep = false; woke++; }
static void display_bump_activity(void) { bumps++; }
static void habitat_input_stamp(int64_t n) { (void)n; }
static void habitat_touch_cancel(void) { canceled++; }
static void habitat_touch(bool down, int x, int y, uint32_t n)
{ (void)n; assert(event_count < 400); events[event_count++] = (event_t){down, x, y}; }
''' + body + r'''
// A 4 ms pass, as the real task runs while a finger is down, for `n` passes. `rc` is what the controller
// answers on every pass.
static void run(int n, int rc, bool sleeping, bool controller_up)
{
    static pass_t list[200];
    for (int i = 0; i < n; i++) list[i] = (pass_t){(uint32_t)(i * 4), rc};
    passes = list; count = n; at = 0; event_count = 0; canceled = woke = bumps = 0;
    atomic_store(&presses, 0);
    asleep = sleeping; open_ok = controller_up; controller = controller_up ? (void *)1 : NULL;
    if (!setjmp(stop)) task(NULL);
}
static int downs(void) { int k = 0; for (int i = 0; i < event_count; i++) k += events[i].down; return k; }
static int ups(void) { return event_count - downs(); }
int main(void)
{
    dbg_gesture_t g;
    // Nothing armed: the hook is one load, and a quiet glass forwards nothing.
    run(40, 0, false, true);
    assert(event_count == 0 && atomic_load(&presses) == 0 && bumps == 0);

    // A tap: it is a real press — counted, the activity timer bumped — and arrives at habitat_touch as
    // contact at the asked point followed by exactly one release there.
    assert(dbg_tap_make(233, 220, &g) == DBG_OK && dbg_inject_arm(&g, 0));
    run(40, 0, false, true);
    assert(atomic_load(&presses) == 1 && bumps > 0 && canceled == 0);
    assert(downs() >= 10 && ups() == 1);
    for (int i = 0; i < event_count; i++) assert(events[i].x == 233 && events[i].y == 220);
    assert(events[0].down && !events[event_count - 1].down);
    assert(dbg_inject_finish(1 << 30) == 1);
    run(40, 0, false, true);
    assert(event_count == 0);   // it plays once

    // The idle controller's stale ACKs (the CST9217 answers INVALID_RESPONSE when nothing touches it) do not
    // cut an injected contact short or cancel it.
    assert(dbg_tap_make(100, 233, &g) == DBG_OK && dbg_inject_arm(&g, 0));
    run(40, ESP_ERR_INVALID_RESPONSE, false, true);
    assert(downs() >= 10 && ups() == 1 && canceled == 0);
    assert(dbg_inject_finish(1 << 30) == 1);

    // A swipe walks from start to end in order, ends on its end point as a contact, then lifts there.
    assert(dbg_swipe_make(100, 233, 300, 233, 100, &g) == DBG_OK && dbg_inject_arm(&g, 0));
    run(50, 0, false, true);
    assert(downs() > 10 && ups() == 1);
    assert(events[0].down && events[0].x == 100 && events[0].y == 233);
    for (int i = 1; i < event_count; i++) assert(events[i].x >= events[i - 1].x && events[i].y == 233);
    assert(events[event_count - 2].down && events[event_count - 2].x == 300);
    assert(!events[event_count - 1].down && events[event_count - 1].x == 300);
    assert(dbg_inject_finish(1 << 30) == 1);

    // Asleep, an injected tap behaves as a finger does: it wakes the panel and is swallowed, not delivered.
    assert(dbg_tap_make(233, 233, &g) == DBG_OK && dbg_inject_arm(&g, 0));
    run(40, 0, true, true);
    assert(woke == 1 && downs() == 0 && ups() == 0);
    assert(dbg_inject_finish(1 << 30) == 1);

    // No controller: the task cannot play it, and says so rather than leaving the host waiting.
    assert(dbg_tap_make(233, 233, &g) == DBG_OK && dbg_inject_arm(&g, 0));
    run(40, 0, false, false);
    assert(event_count == 0 && dbg_inject_finish(1 << 30) == -1);

    puts("touch inject: PASS (production task; tap and swipe reach habitat_touch like a finger, stale ACKs, wake swallow, dead controller, idle cost)");
}
'''
with tempfile.TemporaryDirectory(prefix='harness-touch-inject-') as d:
    root = Path(d)
    (root / 'inject.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(p / '../main/ui/habitat'), '-I', str(p / '../main'),
                    str(root / 'inject.c'), str(p / '../main/debug_cmd.c'),
                    '-o', str(root / 'inject'), '-lm'], check=True)
    subprocess.run([str(root / 'inject')], check=True)
