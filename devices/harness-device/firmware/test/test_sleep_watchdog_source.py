"""Source rules for the IDF-only half of the sleep hang fix (2026-11-01 watchdog hang on dial :50), checked without a toolchain.

The logic that can run on the host does: test_power_sleep_glue.py (arm, feed, disarm order; the D+ bounce rule; the breadcrumb call) and
test_last_words_crumb.py (the record, the report, the watchdog handler). What is left are two files that call IDF hardware functions, and the wiring:

  1. sleep_wdt.c: the RTC watchdog (WDT_RWDT), one stage with a SYSTEM reset (RTC memory survives; RESET_RTC would not be the same promise), every
     register write between write_protect_disable and write_protect_enable, under one spinlock; feed and disarm exist.
  2. usb_bounce.c: all four pulls forced off, 100 ms, then the override released, in that order.
  3. Both files and their callers exist only with DEVICE_BATTERY_SLEEP: CMake adds the sources under the flag; power_sleep.c is the only caller; the
     task watchdog's handler is defined once, in last_words.c, under the flag.
  4. The default image does not name any of it.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    src = re.sub(r'/\*.*?\*/', ' ', text(name), flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)


def body(src, name):
    match = re.search(r'^[^\n;]*\b' + re.escape(name) + r'\([^;{]*?\)\s*\{', src, re.M)
    assert match, f'no definition of {name}'
    depth, i = 0, match.end() - 1
    start = i
    while True:
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1


# ── 1. sleep_wdt.c ──────────────────────────────────────────────────────────────────────────────────
wdt = code('sleep_wdt.c')
assert re.search(r'RWDT_HAL_CONTEXT_DEFAULT\(\)', wdt), 'the RTC watchdog context'
arm = body(wdt, 'sleep_wdt_arm')
assert 'WDT_RWDT' in arm and 'WDT_STAGE_ACTION_RESET_SYSTEM' in arm and 'RESET_RTC' not in arm, 'a system reset, which keeps RTC memory'
assert 'wdt_hal_config_stage(&s_ctx, WDT_STAGE0, ticks,' in arm and 'rtc_clk_slow_freq_get_hz()' in arm, 'the timeout is in slow-clock ticks'
assert arm.index('wdt_hal_config_stage') < arm.index('wdt_hal_enable'), 'configured before it is enabled'
for name in ('sleep_wdt_arm', 'sleep_wdt_feed', 'sleep_wdt_disarm'):
    fn = body(wdt, name)
    assert fn.count('wdt_hal_write_protect_disable(&s_ctx)') == 1 and fn.count('wdt_hal_write_protect_enable(&s_ctx)') == 1, name
    assert fn.index('portENTER_CRITICAL') < fn.index('wdt_hal_write_protect_disable') < fn.index('wdt_hal_write_protect_enable') < fn.index('portEXIT_CRITICAL'), \
        f'{name}: the write-protect window is inside the spinlock'
assert 'wdt_hal_feed(&s_ctx)' in body(wdt, 'sleep_wdt_feed') and 'wdt_hal_disable(&s_ctx)' in body(wdt, 'sleep_wdt_disarm')

# ── 2. usb_bounce.c ─────────────────────────────────────────────────────────────────────────────────
bounce = code('usb_bounce.c')
assert re.search(r'#define BOUNCE_MS 100\b', bounce)
fn = body(bounce, 'usb_dp_bounce')
assert re.search(r'\.dp_pu = false, \.dm_pu = false, \.dp_pd = false, \.dm_pd = false', fn), 'the D+ pull-up is off, and no other pull is on'
assert fn.index('usb_serial_jtag_ll_phy_enable_pull_override(&off)') < fn.index('vTaskDelay(pdMS_TO_TICKS(BOUNCE_MS))') < fn.index('usb_serial_jtag_ll_phy_disable_pull_override()'), \
    'drop the pull-up, wait, give the PHY its own pull-up back'

# ── 3. the flag ─────────────────────────────────────────────────────────────────────────────────────
cmake = text('CMakeLists.txt')
block = re.search(r'if\(DEVICE_BATTERY_SLEEP\)\n    if\(NOT CONFIG_PM_ENABLE.*?\nendif\(\)', cmake, re.S)
assert block and 'target_sources(${COMPONENT_LIB} PRIVATE "power_policy.c" "power_sleep.c" "usb_bounce.c" "sleep_wdt.c")' in block.group(0)
assert cmake.count('usb_bounce.c') == 1 and cmake.count('sleep_wdt.c') == 1, 'listed once, in the flag block, and nowhere else'
users = {'sleep_wdt_arm': 'power_sleep.c', 'sleep_wdt_feed': 'power_sleep.c', 'sleep_wdt_disarm': 'power_sleep.c', 'usb_dp_bounce': 'power_sleep.c'}
for path in sorted(main.rglob('*.c')):
    rel = path.relative_to(main).as_posix()
    if 'managed_components' in path.parts or rel in ('sleep_wdt.c', 'usb_bounce.c'):
        continue
    src = code(rel)
    for fn_name, owner in users.items():
        if re.search(r'\b' + fn_name + r'\(', src):
            assert rel == owner, f'{rel} calls {fn_name}; only {owner} may'
psleep = text('power_sleep.c')
assert '#ifdef DEVICE_BATTERY_SLEEP' in text('power_sleep.h') and 'power_sleep.c' in block.group(0), 'power_sleep.c is built only with the flag'

lw = text('last_words.c')
assert len(re.findall(r'void esp_task_wdt_isr_user_handler\(void\)', lw)) == 1
handler = re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:(?!#endif).*\n)*?(?:.*\n)*?void esp_task_wdt_isr_user_handler\(void\)', lw)
assert handler, 'the watchdog handler sits inside the DEVICE_BATTERY_SLEEP block of last_words.c'
for path in sorted(main.rglob('*.c')):
    rel = path.relative_to(main).as_posix()
    if 'managed_components' not in path.parts and rel != 'last_words.c':
        assert 'esp_task_wdt_isr_user_handler' not in text(rel), f'{rel} defines or names the task watchdog handler'
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n#include "esp_task_wdt.h"\n#endif', lw)
# the handler records through the IDF's own routine, with no log and no lock: nothing in its body may log or take a mutex
h = body(re.sub(r'//[^\n]*', '', lw), 'esp_task_wdt_isr_user_handler')
for banned in ('ESP_LOG', 'xSemaphore', 'portENTER_CRITICAL', 'malloc', 'printf'):
    assert banned not in h, f'the watchdog handler runs in an interrupt: no {banned}'
assert 'esp_task_wdt_print_triggered_tasks(starved_add, &sink, NULL)' in h

# ── 4. the default image ────────────────────────────────────────────────────────────────────────────
disp = text('ui/display.h')
assert re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n(?:[^\n]*\n)*?unsigned display_render_stage\(void\);\n(?:[^\n]*\n)*?#endif', disp), 'declared inside the flag block'
assert 'sleep_wdt' not in text('ptt.c') and 'usb_bounce' not in text('ptt.c')

print('sleep hang fix wiring: RTC watchdog (system reset, write-protect inside the lock), D+ bounce order and timing, sources only with the flag, '
      'one task-watchdog handler (last_words.c) with no log or lock PASS')
