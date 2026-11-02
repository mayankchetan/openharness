"""S2_PANEL_SLPIN: display_panel_sleep() puts the CO5300 in sleep-in for S2 and brings it out again (display_habitat.c).

The production function is cut out of display_habitat.c and run against a fake panel and panel IO. Checked:
  * sleep(true): DISPOFF (through the panel handle), then DCS SLPIN 0x10, then 120 ms; the sleep-in flag is set;
  * sleep(false): DCS SLPOUT 0x11, then 120 ms; the flag is cleared and the "woke" flag is set (the render loop lights the panel after
    the first full frame and sends the brightness again; that part is run by test_display_power.py);
  * the QSPI command word is the one the panel driver builds in its own tx_param: (LCD_OPCODE_WRITE_CMD 0x02 << 24) | (cmd << 8)
    (managed_components/espressif__esp_lcd_co5300/esp_lcd_co5300_spi.c, tx_param, LCD_OPCODE_WRITE_CMD);
  * 120 ms is the driver's CO5300_SLEEP_SETTLE_TIME_MS, used after its own SLPIN and SLPOUT (same file, lines 43, 402 and 421);
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
assert re.search(r'#if S2_PANEL_SLPIN\n(?://[^\n]*\n|#define[^\n]*\n)*void display_panel_sleep\(bool sleep\)\n\{', source), 'the function is compiled only with the switch'
assert '#define PANEL_SETTLE_MS 120' in source and '#define PANEL_DCS(cmd)' in source
assert 'panel_io = io;' in function('display_init'), 'display_init keeps the panel IO handle'
body = function('display_panel_sleep')
assert 'esp_lcd_panel_disp_sleep' not in body and 'DSTBON' not in body and '0X4F' not in body.upper()
assert 'LCD_CMD_SLPIN' in body and 'LCD_CMD_SLPOUT' in body

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
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
enum { RENDER_POWER = 1 };
typedef int esp_err_t;
typedef int esp_lcd_panel_handle_t;
typedef int *esp_lcd_panel_io_handle_t;
static int the_panel = 7, the_io = 9;
static esp_lcd_panel_handle_t panel = 7;
static esp_lcd_panel_io_handle_t panel_io = &the_io;
static char log_[512];
static int fail_next;
static void render_progress(unsigned stage) { assert(stage == RENDER_POWER); strcat(log_, "P"); }
static esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { assert(p == the_panel + 0); strcat(log_, on ? "ON " : "OFF "); return 0; }
static esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t size) {
    assert(io == &the_io && !param && size == 0);
    char t[32]; snprintf(t, sizeof t, "CMD%08x ", (unsigned)cmd); strcat(log_, t);
    if (fail_next) { fail_next = 0; return 1; }
    return 0;
}
static void vTaskDelay(unsigned ticks) { char t[16]; snprintf(t, sizeof t, "D%u ", ticks); strcat(log_, t); }
static atomic_bool s_panel_slpin;
static bool s_panel_woke;
#define PANEL_SETTLE_MS 120
#define PANEL_DCS(cmd) ((int)((0x02u << 24) | ((unsigned)(cmd) << 8)))
''' + function('display_panel_sleep') + r'''
int main(void) {
    // DISPOFF, SLPIN (QSPI word 0x02001000), 120 ms
    display_panel_sleep(true);
    assert(!strcmp(log_, "POFF CMD02001000 D120 ") && atomic_load(&s_panel_slpin) && !s_panel_woke);
    log_[0] = 0; display_panel_sleep(true);  assert(!log_[0]);                    // a repeat sends nothing
    // SLPOUT (QSPI word 0x02001100), 120 ms; the loop lights the panel after the first frame
    display_panel_sleep(false);
    assert(!strcmp(log_, "PCMD02001100 D120 ") && !atomic_load(&s_panel_slpin) && s_panel_woke);
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
with tempfile.TemporaryDirectory(prefix='harness-panel-slpin-') as folder:
    folder = Path(folder)
    (folder / 'p.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds'),
                    str(folder / 'p.c'), '-o', str(folder / 'p')], check=True)
    subprocess.run([str(folder / 'p')], check=True)
