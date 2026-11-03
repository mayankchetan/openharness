"""S2_PANEL_SLPIN: display_panel_sleep() puts the CO5300 in sleep-in for S2 and brings it out again (display_habitat.c).

The production function is cut out of display_habitat.c and run against a fake panel and panel IO. Checked:
  * sleep(true): DISPOFF (through the panel handle), then DCS SLPIN 0x10, then 120 ms; the sleep-in flag is set;
  * sleep(false): DCS SLPOUT 0x11, then 120 ms; the flag is cleared and the "woke" flag is set (the render loop lights the panel after
    the first full frame and sends the brightness again; that part is run by test_display_power.py);
  * the QSPI command word is the one the panel driver builds in its own tx_param: (LCD_OPCODE_WRITE_CMD 0x02 << 24) | (cmd << 8)
    (managed_components/espressif__esp_lcd_co5300/esp_lcd_co5300_spi.c, tx_param, LCD_OPCODE_WRITE_CMD);
  * 120 ms is the driver's CO5300_SLEEP_SETTLE_TIME_MS, used after its own SLPIN and SLPOUT (same file, lines 43, 402 and 421);
  * after the sleep-out and its 120 ms, before the render loop lights the panel, the page-0x20 part of init_cmds is sent again (FE 20, 19 10,
    1C A0, FE 00): the CO5300 datasheet (7.5.12 SLPOUT, p.145) says the sleep-out loads the default values of the extended commands into the
    registers, and those two are extended registers the init set. The slice is read from the init_cmds table itself (the first FE entry with
    data 0x20 up to the next FE entry with data 0x00), not copied by hand. The standard registers (3A, 53, 51, 63, 36) are not sent: the
    datasheet lists no reset for them on sleep-in or sleep-out;
  * a repeat of either call, and a wake that never slept, send nothing;
  * the deep standby of esp_lcd_panel_disp_sleep() is NOT used (see the source comment for why): no 0x4F command is ever sent;
  * with the switch off nothing is compiled.
Source rules: the function is behind S2_PANEL_SLPIN and DEVICE_BATTERY_SLEEP, runs on the render task only (power_sleep.c calls it from
enter_s2 / exit_s2), and the panel IO handle is kept at display_init.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent / '..'
main = root / 'main'
source = (main / 'ui/habitat/display_habitat.c').read_text()
display_h = (main / 'ui/display.h').read_text()


def function(name):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


switches = (main / 's2_switches.h').read_text()
assert re.search(r'#ifndef S2_PANEL_SLPIN\n#define S2_PANEL_SLPIN 0\n#endif', switches) and '#undef S2_PANEL_SLPIN' in switches
assert '#include "s2_switches.h"' in source
cmake = (main / 'CMakeLists.txt').read_text()
assert 'S2_PANEL_SLPIN' in re.search(r'foreach\(s2_switch ([A-Z0-9_ ]+)\)', cmake).group(1).split()
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?void display_panel_sleep\(bool sleep\);', display_h)
assert re.search(r'#if S2_PANEL_SLPIN\n(?://[^\n]*\n|#define[^\n]*\n|static void panel_replay_ext_page\(void\)\n\{.*?\n\}\n)*void display_panel_sleep\(bool sleep\)\n\{', source, re.S), 'the function is compiled only with the switch'
assert '#define PANEL_SETTLE_MS 120' in source and '#define PANEL_DCS(cmd)' in source
assert 'panel_io = io;' in function('display_init'), 'display_init keeps the panel IO handle'
body = function('display_panel_sleep')
assert 'esp_lcd_panel_disp_sleep' not in body and 'DSTBON' not in body and '0X4F' not in body.upper()
assert 'LCD_CMD_SLPIN' in body and 'LCD_CMD_SLPOUT' in body
# the replay reads the init_cmds table; no register value is copied into either function
replay = function('panel_replay_ext_page')
assert 'init_cmds' in replay and 'panel_replay_ext_page()' in body
assert not re.search(r'0x[0-9a-fA-F]+', replay), 'no hand-copied register in the replay'
assert not re.search(r'0x[0-9a-fA-F]+', body), 'no register value in the sleep function'
table = re.search(r'static const co5300_lcd_init_cmd_t init_cmds\[\] = \{\n.*?\n\};\n', source, re.S)
assert table, 'init_cmds table'
page_defs = ''.join(re.search(r'^#define ' + n + r'[^\n]*\n', source, re.M).group(0) for n in ('PANEL_PAGE_CMD', 'PANEL_PAGE_EXT', 'PANEL_PAGE_USER'))
assert source.index('static const co5300_lcd_init_cmd_t init_cmds[]') < source.index('void display_panel_sleep(bool sleep)\n{')

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define DEVICE_BATTERY_SLEEP 1
#define S2_PANEL_SLPIN 1
#define LCD_CMD_SLPIN 0x10
#define LCD_CMD_SLPOUT 0x11
#define pdMS_TO_TICKS(n) (n)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
enum { RENDER_POWER = 1 };
typedef int esp_err_t;
typedef int esp_lcd_panel_handle_t;
typedef int *esp_lcd_panel_io_handle_t;
#ifndef EXPECT_1C
#define EXPECT_1C "a0"
#endif
static int the_panel = 7, the_io = 9;
static esp_lcd_panel_handle_t panel = 7;
static esp_lcd_panel_io_handle_t panel_io = &the_io;
static char log_[1024];
static int fail_next;
static void render_progress(unsigned stage) { assert(stage == RENDER_POWER); strcat(log_, "P"); }
static esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { assert(p == the_panel + 0); strcat(log_, on ? "ON " : "OFF "); return 0; }
static esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t size) {
    assert(io == &the_io && (param != NULL) == (size != 0));
    char t[32]; snprintf(t, sizeof t, "CMD%08x ", (unsigned)cmd); strcat(log_, t);
    for (size_t i = 0; i < size; i++) { snprintf(t, sizeof t, "%02x ", ((const uint8_t *)param)[i]); strcat(log_, t); }
    if (fail_next) { fail_next = 0; return 1; }
    return 0;
}
static void vTaskDelay(unsigned ticks) { char t[16]; snprintf(t, sizeof t, "D%u ", ticks); strcat(log_, t); }
static atomic_bool s_panel_slpin;
static bool s_panel_woke;
#define PANEL_SETTLE_MS 120
#define PANEL_DCS(cmd) ((int)((0x02u << 24) | ((unsigned)(cmd) << 8)))
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned int delay_ms; } co5300_lcd_init_cmd_t;
''' + page_defs + table.group(0) + function('panel_replay_ext_page') + function('display_panel_sleep') + r'''
int main(void) {
    // DISPOFF, SLPIN (QSPI word 0x02001000), 120 ms
    display_panel_sleep(true);
    assert(!strcmp(log_, "POFF CMD02001000 D120 ") && atomic_load(&s_panel_slpin) && !s_panel_woke);
    log_[0] = 0; display_panel_sleep(true);  assert(!log_[0]);                    // a repeat sends nothing
    // SLPOUT (QSPI word 0x02001100), 120 ms; the loop lights the panel after the first frame
    display_panel_sleep(false);
    // then the page-0x20 slice of init_cmds again: FE 20, 19 10, 1C A0, FE 00 (no delay entries in it), and nothing else
    assert(!strcmp(log_, "PCMD02001100 D120 CMD0200fe00 20 CMD02001900 10 CMD02001c00 " EXPECT_1C " CMD0200fe00 00 ") && !atomic_load(&s_panel_slpin) && s_panel_woke);
    assert(!strstr(log_, "CMD02003a00") && !strstr(log_, "CMD02005300") && !strstr(log_, "CMD02005100") && !strstr(log_, "CMD02006300") && !strstr(log_, "CMD02003600"));
    log_[0] = 0; display_panel_sleep(false); assert(!log_[0]);
    // a wake that never slept: nothing, and the woke flag is not raised
    s_panel_woke = false; atomic_store(&s_panel_slpin, false);
    display_panel_sleep(false); assert(!log_[0] && !s_panel_woke);
    // the flags stay in step across many cycles
    for (int i = 0; i < 100; i++) {
        log_[0] = 0;
        display_panel_sleep(true); assert(atomic_load(&s_panel_slpin));
        display_panel_sleep(false); assert(!atomic_load(&s_panel_slpin) && s_panel_woke); s_panel_woke = false;
        assert(!strstr(log_, "4f") && !strstr(log_, "4F"));                           // never DSTBON (0x4F)
    }
    puts("panel sleep-in: DISPOFF, SLPIN, 120 ms and SLPOUT, 120 ms; QSPI word as the driver builds it; repeats and a wake without a sleep send nothing; no deep standby PASS");
    return 0;
}
'''


def build_and_run(folder, name, text, defines=()):
    (folder / (name + '.c')).write_text(text)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    *defines, str(folder / (name + '.c')), '-o', str(folder / name)], check=True)
    return subprocess.run([str(folder / name)], check=True, capture_output=True, text=True).stdout


with tempfile.TemporaryDirectory(prefix='harness-panel-slpin-') as folder:
    folder = Path(folder)
    print(build_and_run(folder, 'p', code), end='')
    # The replay follows the table: change one value in init_cmds (1C A0 -> 1C B1) and the replayed command changes with it, no other edit.
    assert '{0x1C, (uint8_t[]){0xA0}, 1, 0}' in code
    print(build_and_run(folder, 'q', code.replace('{0x1C, (uint8_t[]){0xA0}, 1, 0}', '{0x1C, (uint8_t[]){0xB1}, 1, 0}'), ['-DEXPECT_1C="b1"']).replace('PASS', 'PASS (table edit followed)'), end='')
    # A table without the page select back to 0 stops the replay before anything is sent (it must not run on into SLPOUT / DISPON).
    broken = code.replace('{0xFE, (uint8_t[]){0x00}, 1, 0},\n    {0xC4', '{0xC4')
    assert broken != code
    try:
        build_and_run(folder, 'r', broken)
    except subprocess.CalledProcessError:
        print('a table without the page select back to 0 aborts the replay PASS')
    else:
        raise AssertionError('the replay ran without its end marker')
