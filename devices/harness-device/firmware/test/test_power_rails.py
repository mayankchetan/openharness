"""The AXP2101 rail registers in the boot log (2026-11-02): read only, one line.

  * power_rails_format() (board/power.h, pure) turns the 27 raw bytes of REG 0x80..0x9A into one log line: "AXP2101 rails (read only): 80=xx 81=xx ... 9a=xx",
    two hex digits each, "--" for a register that did not read. 0x80 and 0x81 are the DC-DC enable and mode registers, 0x90 and 0x91 the LDO enables,
    0x82..0x9A the DC-DC and LDO voltage registers (datasheet V1.4 pages 50-53; briefs/research/2026-10-29-axp2101-registers.md);
  * power_log_rails() (board/power.c) only reads: it calls axp_read and never axp_write, never a rail register write; a board without the PMIC reads nothing;
  * app_main calls it once at boot, after config_store_init.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
power_h = (main / 'board/power.h').read_text()
power_c = (main / 'board/power.c').read_text()
app = (main / 'app_main.c').read_text()

fn = re.search(r'void power_log_rails\(void\)\n\{.*?^\}', power_c, re.M | re.S)
assert fn, 'power_log_rails in power.c'
body = fn.group(0)
assert 'axp_read(' in body and 'axp_write' not in body and 'power_pmic_soft_off' not in body, 'read only'
assert 'board()->has_pmic' in body, 'no PMIC, no reads'
assert 'power_rails_format(' in body
assert re.search(r'void power_log_rails\(void\);', power_h)
assert re.search(r'power_off_boot_report\(\);[^\n]*\n\s*power_log_rails\(\);', app), 'called once at boot, after power_off_boot_report / config_store_init'
fmt = re.search(r'static inline int power_rails_format\(.*?\n\}', power_h, re.S)
assert fmt, 'the formatter is pure and in power.h'
defs = re.findall(r'#define POWER_RAILS_(?:FIRST|LAST|COUNT)\b[^\n]*', power_h)
assert len(defs) == 3

code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board/power.h"
int main(void) {
    assert(POWER_RAILS_FIRST == 0x80 && POWER_RAILS_LAST == 0x9A && POWER_RAILS_COUNT == 27);
    int regs[POWER_RAILS_COUNT];
    for (int i = 0; i < POWER_RAILS_COUNT; i++) regs[i] = i == 0 ? 0xFF : i == 1 ? 0x00 : 0x10 + i;
    char line[256];
    int n = power_rails_format(line, sizeof line, regs);
    assert(n == (int)strlen(line) && n < 200);
    assert(!strncmp(line, "AXP2101 rails (read only): 80=ff 81=00 82=12 ", 45));
    assert(strstr(line, " 90=20 91=21 ") && strstr(line, " 9a=2a") && line[n - 1] != ' ');
    regs[2] = -1; regs[0x10] = -1;                                   /* failed reads */
    power_rails_format(line, sizeof line, regs);
    assert(strstr(line, " 82=-- ") && strstr(line, " 90=-- "));
    char small[20];                                                  /* a small buffer is cut, not overrun */
    n = power_rails_format(small, sizeof small, regs);
    assert(n < (int)sizeof small && strlen(small) == (size_t)n);
    puts("AXP2101 rails line: 27 registers, 2 hex digits, -- for a failed read, small buffer safe PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-power-rails-') as folder:
    folder = Path(folder)
    (folder / 'r.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    '-I', str(main), str(folder / 'r.c'), '-o', str(folder / 'r')], check=True)
    subprocess.run([str(folder / 'r')], check=True)
