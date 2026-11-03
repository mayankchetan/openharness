"""The S2 wake-diagnosis image (2026-11-02): a build option, never in a product image.

  * -DDEVICE_S2_DIAG=1 (main/CMakeLists.txt, default OFF) and the overlay sdkconfig.defaults.diag (CONFIG_PM_PROFILING, CONFIG_ESP_TIMER_PROFILING; every name is
    checked against the Kconfig files of the ESP-IDF in $IDF_PATH when it is set) are the only way it gets in. Without the flag no byte of it is compiled:
    every use of s2_diag.h, the wake-cause counter and the dump sits inside #ifdef DEVICE_S2_DIAG;
  * power_sleep_diag_dump() (power_sleep.c) runs once per alive period from the render task and only while the chip may sleep (S2): it logs the wake
    causes since the last dump, esp_pm_dump_locks and esp_timer_dump, line by line through the log (not to stdout, which is the cable's console);
  * s2_diag.h is pure: the wake-cause counter and the line formatter run here on the host. Cause names are indexed by esp_sleep_wakeup_cause_t (IDF 5.5.6).
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'


def text(rel):
    return (main / rel).read_text(encoding='utf-8').replace('\r\n', '\n')


cmake = text('CMakeLists.txt')
assert re.search(r'if\(DEVICE_S2_DIAG\)\n\s*target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE DEVICE_S2_DIAG=1\)', cmake), 'cmake switch, default off'
assert 'DEVICE_BATTERY_SLEEP' in cmake[cmake.index('DEVICE_S2_DIAG') - 400:cmake.index('DEVICE_S2_DIAG')] or 'needs DEVICE_BATTERY_SLEEP' in cmake

overlay = (fw / 'sdkconfig.defaults.diag').read_text()
names = re.findall(r'^(CONFIG_[A-Z0-9_]+)=y', overlay, re.M)
assert {'CONFIG_PM_PROFILING', 'CONFIG_ESP_TIMER_PROFILING'} <= set(names)
idf = os.environ.get('IDF_PATH')
if idf:
    kconfigs = ''.join(p.read_text(errors='replace') for p in Path(idf).rglob('Kconfig*') if p.is_file() and 'examples' not in p.parts and 'tools' not in p.parts)
    for n in names:
        assert re.search(r'^\s*config ' + n[len('CONFIG_'):] + r'\b', kconfigs, re.M), n + ' is not a Kconfig name of this ESP-IDF'

# every use sits inside the flag
for rel in ('power_sleep.c', 'power_sleep.h', 'ui/habitat/display_habitat.c'):
    src = text(rel)
    for m in re.finditer(r'power_sleep_diag_dump|s2_diag\.h|s2d_note|s_diag_', src):
        before = src[:m.start()]
        opens = len(re.findall(r'^#if(?:def)? ', before, re.M)) - len(re.findall(r'^#endif', before, re.M))
        last_if = [x for x in re.findall(r'^#(?:ifdef|if|ifndef|else|endif)[^\n]*', before, re.M)]
        assert 'DEVICE_S2_DIAG' in '\n'.join(last_if[-3:]) or opens > 0, f'{rel}: a diag name outside the flag'
dump = re.search(r'void power_sleep_diag_dump\(void\)\n\{.*?^\}', text('power_sleep.c'), re.M | re.S)
assert dump, 'power_sleep_diag_dump'
d = dump.group(0)
assert 'power_sleep_relaxed()' in d, 'only while the chip may sleep'
assert 'esp_pm_dump_locks(' in d or 'esp_pm_dump_locks' in text('power_sleep.c')
assert 'esp_timer_dump' in text('power_sleep.c') and 'open_memstream' in text('power_sleep.c'), 'dumps go to a memory stream, then to the log'
assert 'stdout' not in text('power_sleep.c')
assert re.search(r'#ifdef DEVICE_S2_DIAG\n\s*power_sleep_diag_dump\(\);[^\n]*\n#endif', text('ui/habitat/display_habitat.c')), 'called from the alive report'
assert re.search(r's2d_note\(', text('power_sleep.c')) and 'esp_sleep_get_wakeup_causes()' in text('power_sleep.c')

code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "s2_diag.h"
int main(void) {
    assert(S2D_CAUSES == 18);
    assert(!strcmp(s2d_cause_name(4), "TIMER") && !strcmp(s2d_cause_name(7), "GPIO") && !strcmp(s2d_cause_name(11), "WIFI") && !strcmp(s2d_cause_name(14), "BT"));
    assert(!strcmp(s2d_cause_name(0), "NONE") && !strcmp(s2d_cause_name(S2D_CAUSES), "?"));
    uint32_t c[S2D_CAUSES] = {0};
    s2d_note(c, 1u << 4);                          /* a timer wake */
    s2d_note(c, 1u << 4);
    s2d_note(c, (1u << 11) | (1u << 4));           /* two causes at once: each is counted */
    s2d_note(c, 0);                                /* none reported */
    s2d_note(c, 1u << 30);                         /* a bit beyond the table is ignored */
    assert(c[4] == 3 && c[11] == 1 && c[0] == 1);
    uint32_t base[S2D_CAUSES] = {0};
    base[4] = 1;
    char line[200];
    int n = s2d_format(line, sizeof line, c, base, 60000);
    assert(n == (int)strlen(line));
    assert(strstr(line, "wake causes in 60.0 s:") && strstr(line, "TIMER=2 (0.03/s)") && strstr(line, "WIFI=1 (0.01/s)") && strstr(line, "NONE=1"));
    assert(!strstr(line, "GPIO"));                  /* zero counts are left out */
    memset(base, 0, sizeof base); memcpy(base, c, sizeof c);
    n = s2d_format(line, sizeof line, c, base, 10000);
    assert(strstr(line, "wake causes in 10.0 s: none"));
    n = s2d_format(line, 12, c, c, 1000);          /* a small buffer is cut, never overrun */
    assert(n < 12 && (int)strlen(line) == n);
    puts("s2 diag: wake-cause counter and line PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-s2-diag-') as folder:
    folder = Path(folder)
    (folder / 'd.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(main), str(folder / 'd.c'), '-o', str(folder / 'd')], check=True)
    subprocess.run([str(folder / 'd')], check=True)
    if idf:   # the cause numbers the names are indexed by are the enum of this ESP-IDF
        hdr = (Path(idf) / 'components/esp_hw_support/include/esp_sleep.h').read_text()
        enum = re.search(r'typedef enum \{(.*?)\} esp_sleep_source_t;', hdr, re.S)[1]
        order = re.findall(r'^\s*(ESP_SLEEP_WAKEUP_\w+)', enum, re.M)
        want = {'TIMER': 'ESP_SLEEP_WAKEUP_TIMER', 'GPIO': 'ESP_SLEEP_WAKEUP_GPIO', 'WIFI': 'ESP_SLEEP_WAKEUP_WIFI', 'BT': 'ESP_SLEEP_WAKEUP_BT', 'UART': 'ESP_SLEEP_WAKEUP_UART'}
        got = dict((n, int(i)) for n, i in re.findall(r'"(\w+)"\s*,?\s*/\*\s*(\d+)\s*\*/', (main / 's2_diag.h').read_text()))
        for name, sym in want.items():
            assert order.index(sym) == got[name], f'{name} is cause {order.index(sym)} in this IDF'
