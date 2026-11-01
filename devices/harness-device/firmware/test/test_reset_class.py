"""W15e: reset_class() (mode_glue.c) run, not only read. The panic loop counts a reset as a crash or not by this mapping, and test_mode_glue_source.py
only checks its text. Here the real function body is cut out of mode_glue.c, compiled against the SDK's own esp_reset_reason_t (read from IDF_PATH,
as the source test does) and the real ms_reset_t (mode_switch.h), and called for EVERY reason the SDK defines plus numbers it does not.

The expected table is the user decision of 2026-10-31 and its recorded deviation: power-on zeroes the record; SW, DEEPSLEEP, EXT, USB and JTAG hold the
count (somebody asked for the reset: a flash tool, a reset pin, a debugger); every other reason is a crash, and so is a reason a later SDK adds.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'

idf = os.environ.get('IDF_PATH')
header = Path(idf) / 'components/esp_system/include/esp_system.h' if idf else None
if not header or not header.exists():
    print('reset_class behaviour: SKIPPED (IDF_PATH is not set or has no esp_system.h; run.sh sets it for the SDK tests)')
    sys.exit(0)

sdk = header.read_text()
enum_body = re.search(r'typedef enum \{(.*?)\} esp_reset_reason_t;', sdk, re.S)[1]
enum_body = re.sub(r'/\*.*?\*/', '', enum_body, flags=re.S)
enum_body = re.sub(r'//[^\n]*', '', enum_body)
reasons = re.findall(r'\b(ESP_RST_\w+)\b', enum_body)
assert len(reasons) >= 10 and 'ESP_RST_USB' in reasons, reasons

glue = (main / 'mode_glue.c').read_text(encoding='utf-8').replace('\r\n', '\n')
match = re.search(r'^static ms_reset_t reset_class\(esp_reset_reason_t reason\)\n\{.*?\n\}', glue, re.S | re.M)
assert match, 'reset_class'

HOLD = {'ESP_RST_SW', 'ESP_RST_DEEPSLEEP', 'ESP_RST_EXT', 'ESP_RST_USB', 'ESP_RST_JTAG'}
POWERON = {'ESP_RST_POWERON'}
cases = '\n'.join(
    f'    check({r}, {"MS_RESET_POWERON" if r in POWERON else "MS_RESET_HOLD" if r in HOLD else "MS_RESET_CRASH"}, "{r}");' for r in reasons)

driver = f'''
#include <stdio.h>
#include <stdlib.h>
#include "mode_switch.h"
typedef enum {{{enum_body}}} esp_reset_reason_t;
{match[0]}
static int failures;
static void check(esp_reset_reason_t r, ms_reset_t want, const char *name)
{{
    if (reset_class(r) != want) {{ printf("FAIL %s -> %d, want %d\\n", name, (int)reset_class(r), (int)want); failures++; }}
}}
int main(void)
{{
{cases}
    /* a reason a later SDK adds, or a corrupt number: a crash (the safe side for the count is "believed", and the loop bound is the point) */
    check((esp_reset_reason_t)99, MS_RESET_CRASH, "unknown 99");
    check((esp_reset_reason_t)-1, MS_RESET_CRASH, "unknown -1");
    /* the one that decided the deviation: an esptool flash logged "reset reason: usb" and must not count */
    check(ESP_RST_USB, MS_RESET_HOLD, "ESP_RST_USB");
    check(ESP_RST_PANIC, MS_RESET_CRASH, "ESP_RST_PANIC");
    return failures ? 1 : 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'reset_class.c'
    c.write_text(driver)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=undefined,bounds', '-I', str(main), '-o', str(Path(tmp) / 'rc'), str(c)], check=True)
    run = subprocess.run([str(Path(tmp) / 'rc')], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr

print(f'reset_class behaviour: {len(reasons)} SDK reasons plus two unknown numbers classified as the decision says (power-on, hold: SW DEEPSLEEP EXT USB JTAG, the rest crash) PASS')
