"""main/board/sleep_pins.h: the light-sleep and deep-sleep state of every pad (battery sleep plan section 1.2).

With tickless idle ESP-IDF isolates every GPIO in light sleep, so an output the AMOLED or the amplifier depends on must be
listed as KEEP or it floats. This test fails when:
  * a pin in pins_dial.h has no row (a new pin cannot be forgotten), or a row names a pin that is not there,
  * the LCD chip select, the two reset pins or the amplifier enable is not KEEP in light sleep and held in deep sleep,
  * the touch interrupt is not the wake source,
  * a pin held in deep sleep is an input, or a KEEP pin is not an output,
  * two rows share a GPIO,
  * the table disagrees with the board table (reset pins) or, when the file is reachable, with the workbench parts file,
  * power_sleep.c does not apply gpio_sleep_sel_dis() to the KEEP rows through the table.
"""
from pathlib import Path
import os
import re

fw = Path(__file__).resolve().parent / '..'
pins_h = (fw / 'main/board/pins_dial.h').read_text()
table_h = (fw / 'main/board/sleep_pins.h').read_text()
glue = (fw / 'main/power_sleep.c').read_text()
board_table = (fw / 'main/board/board_table.c').read_text()

# --- the pins that exist: every numeric BSP_ define that is a GPIO ---
NOT_PINS = {'BSP_LCD_H_RES', 'BSP_LCD_V_RES', 'BSP_LCD_BIT_PER_PIXEL', 'BSP_I2C_PORT', 'BSP_I2C_FREQ_HZ', 'BSP_AXP2101_I2C_ADDR'}
pins = {m[1]: int(m[2]) for m in re.finditer(r'^#define\s+(BSP_[A-Z0-9_]+)\s+(\d+)', pins_h, re.M) if m[1] not in NOT_PINS}
assert len(pins) == 16, pins   # the numeric BSP_ pins in pins_dial.h today; a new one fails the next check until it has a row

# --- the table ---
rows = re.findall(r'^\s*\{"([^"]+)",\s*(\w+),\s*(SLEEP_DIR_\w+),\s*(SLEEP_LIGHT_\w+),\s*(SLEEP_DEEP_\w+)\},', table_h, re.M)
assert rows, 'no rows in sleep_pins.h'
by_macro = {}
for name, macro, direction, light, deep in rows:
    assert macro not in by_macro, f'{macro} listed twice'
    by_macro[macro] = (name, direction, light, deep)

BOARD = {'SLEEP_PIN_LCD_RST', 'SLEEP_PIN_TOUCH_RST'}
missing = [n for n in pins if n not in by_macro]
assert not missing, f'pins_dial.h pins with no row in sleep_pins.h: {missing}'
unknown = [m for m in by_macro if m not in pins and m not in BOARD]
assert not unknown, f'rows naming pins that pins_dial.h does not define: {unknown}'
assert BOARD <= set(by_macro), 'the two board-dependent reset rows are missing'

# --- the reset pins: the table's constants are the CST9217 dial's (the only dial battery sleep runs on) ---
lcd_rst = int(re.search(r'#define SLEEP_CST9217_LCD_RST\s+(\d+)', table_h)[1])
touch_rst = int(re.search(r'#define SLEEP_CST9217_TOUCH_RST\s+(\d+)', table_h)[1])
cst9217_branch = re.search(r'\} else \{\s*out->lcd_rst = (\d+); out->touch_rst = (\d+);', board_table)
assert cst9217_branch, 'board_table.c: CST9217 branch not found'
assert (lcd_rst, touch_rst) == (int(cst9217_branch[1]), int(cst9217_branch[2])), 'sleep_pins.h reset pins differ from board_table.c'

gpio = {macro: pins[macro] for macro in pins}
gpio['SLEEP_PIN_LCD_RST'] = lcd_rst
gpio['SLEEP_PIN_TOUCH_RST'] = touch_rst
seen = {}
for macro, number in gpio.items():
    assert number not in seen, f'GPIO{number} is both {seen[number]} and {macro}'
    seen[number] = macro
    assert macro in by_macro

def row(number):
    return by_macro[seen[number]]

# --- the rules ---
for number in (12, 39, 40, 46):                      # LCD chip select, LCD reset, touch reset, amplifier enable
    name, direction, light, deep = row(number)
    assert light == 'SLEEP_LIGHT_KEEP', f'GPIO{number} ({name}) must be KEEP in light sleep'
    assert deep in ('SLEEP_DEEP_HOLD_HIGH', 'SLEEP_DEEP_HOLD_LOW'), f'GPIO{number} ({name}) must be held in deep sleep'
    assert direction == 'SLEEP_DIR_OUT', f'GPIO{number} ({name}) is an output'
assert row(12)[3] == 'SLEEP_DEEP_HOLD_HIGH', 'the chip select is held HIGH (a floating CS on the powered panel takes stray clocks as commands)'
assert row(39)[3] == 'SLEEP_DEEP_HOLD_HIGH' and row(40)[3] == 'SLEEP_DEEP_HOLD_HIGH', 'resets are held high (out of reset)'
assert row(46)[3] == 'SLEEP_DEEP_HOLD_LOW', 'the amplifier stays off'
name, direction, light, deep = row(11)
assert light == 'SLEEP_LIGHT_WAKE_LOW' and deep == 'SLEEP_DEEP_WAKE_LOW' and direction == 'SLEEP_DIR_IN', 'touch INT is the wake pin'
wake = [m for m, (n, d, l, dp) in by_macro.items() if l == 'SLEEP_LIGHT_WAKE_LOW']
assert wake == ['BSP_TOUCH_INT'], f'the only light-sleep wake pin is the touch interrupt: {wake}'
for macro, (name, direction, light, deep) in by_macro.items():
    if deep in ('SLEEP_DEEP_HOLD_HIGH', 'SLEEP_DEEP_HOLD_LOW', 'SLEEP_DEEP_HOLD_IDLE'):
        assert direction == 'SLEEP_DIR_OUT', f'{name}: only an output can be held'
    if light == 'SLEEP_LIGHT_KEEP':
        assert direction == 'SLEEP_DIR_OUT', f'{name}: KEEP is for outputs'
# the BOOT strapping pin: never a wake pin in this stage
assert row(0)[2] == 'SLEEP_LIGHT_ISOLATE' and row(0)[3] == 'SLEEP_DEEP_ISOLATE'
# the panel's data lines go with the chip select
for number in (4, 5, 6, 7, 38):
    assert row(number)[2] == 'SLEEP_LIGHT_KEEP', f'GPIO{number}: panel bus line stays driven'

# --- the glue applies the table, nothing hand-listed ---
assert re.search(r'for \(int i = 0; i < SLEEP_PIN_COUNT; i\+\+\) \{\s*if \(SLEEP_PINS\[i\]\.light != SLEEP_LIGHT_KEEP\) continue;', glue)
code = re.sub(r'//[^\n]*', '', glue)
assert code.count('gpio_sleep_sel_dis(') == 1, 'one call site, driven by the table'
assert 'keep_output_pads();' in glue.split('void power_sleep_init(void)')[1].split('}')[0]

# --- the workbench parts file, when it is reachable (it lives in another repository) ---
parts = os.environ.get('SLEEP_PINS_PARTS_FILE') or 'E:/vibeCodedProjects/workbench/parts/waveshare-esp32-s3-touch-amoled-1.75.md'
checked = 'parts file not reachable: skipped'
if Path(parts).is_file():
    text = Path(parts).read_text(encoding='utf-8')
    listed = {int(m[2]): m[1].strip() for m in re.finditer(r'^\|\s*([A-Z_0-9]+)\s*\|\s*(\d+)\s*\|', text, re.M)}
    assert {11, 40, 0} <= set(listed), f'parts file rows: {listed}'
    for number in listed:
        assert number in seen, f'parts file lists GPIO{number} ({listed[number]}) but the sleep table does not'
    assert listed[11] == 'TP_INT' and listed[40] == 'TP_RST' and listed[0] == 'BOOT'
    checked = f'agrees with the parts file rows {sorted(listed)}'

print(f'sleep pins: {len(rows)} rows cover every pin in pins_dial.h ({len(pins)}) plus the two board resets; '
      f'CS/resets/amp KEEP + held, touch INT the only wake pin, glue driven by the table; {checked} PASS')
