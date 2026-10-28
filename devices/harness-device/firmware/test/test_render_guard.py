"""Fault-inject time into the production display progress monitor, without hardware."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / '../main/ui/habitat/display_habitat.c').read_text()


def function(name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static uint32_t clock_ms;
static unsigned restarts;
static char message[160];
static atomic_uint render_stage, render_progress_ms;
static bool advance_other_core;
static uint32_t now_ms(void) {
    uint32_t now = clock_ms;
    // Another core advances progress between clock sampling and a later load.
    if (advance_other_core) atomic_store(&render_progress_ms, now + 1);
    return now;
}
#define ESP_LOGE(tag, fmt, ...) snprintf(message,sizeof message,fmt,__VA_ARGS__)
#define abort() (++restarts)
'''
code += source[source.index('enum { RENDER_MODEL'):source.index('static atomic_uint render_stage')]
if 'static uint32_t elapsed_since(' in source:
    code += function('elapsed_since')
code += function('render_progress') + function('render_watch')
code += r'''
int main(void) {
    clock_ms=1000; atomic_store(&render_progress_ms,clock_ms);
    advance_other_core=true; render_watch(NULL); assert(!restarts);
    advance_other_core=false;
    // Idle/sleep wakeups and normal frame stages keep making progress.
    for (unsigned stage=RENDER_MODEL; stage<=RENDER_WAIT; stage++) {
        for (unsigned i=0;i<100;i++) {
            render_progress(stage); clock_ms+=1000; render_watch(NULL);
            assert(!restarts);
        }
        // Every stage can block, including a panel call before DMA wait.
        render_progress(stage); clock_ms+=3999; render_watch(NULL); assert(!restarts);
        clock_ms++; render_watch(NULL); assert(restarts==1);
        assert(strstr(message,"renderer stalled 4000 ms")); restarts=0;
    }
    // Clock wrap is ordinary uptime, not a multi-billion-millisecond stall.
    clock_ms=UINT32_MAX-500; render_progress(RENDER_WAIT);
    clock_ms=499; render_watch(NULL); assert(!restarts);
    clock_ms=3499; render_watch(NULL); assert(restarts==1);
    puts("render guard: cross-core timestamp race, every stage, idle/sleep, timeout boundary and clock wrap PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='harness-render-guard-') as folder:
    folder = Path(folder)
    (folder / 'guard.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'), str(folder / 'guard.c'),
                    '-o', str(folder / 'guard')], check=True)
    subprocess.run([str(folder / 'guard')], check=True)


# The render task's stack: 10,240 B when no illustrated companion can draw (the ROM inflater is dead code then),
# 24,576 B when it can. One named constant, used by the task create.
stack = re.search(r'#ifdef HABITAT_NO_COMPANION_ART\s+#define RENDER_STACK_BYTES (\d+)\s+#else\s+#define RENDER_STACK_BYTES (\d+)\s+#endif',
                  source)
assert stack and (stack[1], stack[2]) == ('10240', '24576'), 'RENDER_STACK_BYTES block'
assert re.search(r'xTaskCreatePinnedToCore\(render_task, "habitat_render", RENDER_STACK_BYTES,', source)
for flag, expect in (([], '24576'), (['-DHABITAT_NO_COMPANION_ART=1'], '10240')):
    with tempfile.TemporaryDirectory(prefix='harness-render-stack-') as folder:
        probe = Path(folder) / 'stack.c'
        probe.write_text(stack[0] + '\nint render_stack_probe = RENDER_STACK_BYTES;\n')
        expanded = subprocess.run(['cc', '-E', '-P'] + flag + [str(probe)], check=True, capture_output=True, text=True).stdout
        assert f'render_stack_probe = {expect};' in expanded, (flag, expanded)
print('render stack: RENDER_STACK_BYTES is 10240 without companion art and 24576 with it PASS')
