"""D1 "PWR only, always" (plan section 16): on the battery, with the screen off, only the PWR key wakes the dial.

The behaviour is proved on the host by test_power_policy.c (pp_screen_input_allowed), test_power_sleep_glue.py (the policy state seen by
power_sleep_input_allowed, no touch wake armed in S2) and test_touch_driver.py (a dropped contact). This file pins the wiring those tests
cannot see: which call sites ask the gate, and that the arming code is compiled out under D1.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'
glue = (main / 'power_sleep.c').read_text()
touch = (main / 'ui/habitat/touch_habitat.c').read_text()
ptt = (main / 'ptt.c').read_text()
header = (main / 'power_sleep.h').read_text()
policy_h = (main / 'power_policy.h').read_text()

# the touch level wake is armed only when the build says so, and the default says no
assert re.search(r'#ifndef PP_TOUCH_WAKE_ON_BATTERY\n#define PP_TOUCH_WAKE_ON_BATTERY 0\n#endif', policy_h)
go_live = re.search(r'static bool s2_go_live\(void\)\n\{.*?^\}', glue, re.M | re.S)[0]
assert re.search(r'#if PP_TOUCH_WAKE_ON_BATTERY\n    if \(!touch_sleep_arm\(\)\)', go_live), 'the arm call is behind the macro'
assert glue.count('touch_sleep_arm()') == 1 and glue.count('touch_sleep_disarm()') == 1
for call in ('touch_sleep_disarm();', 'touch_sleep_armed()'):
    i = glue.index(call)
    assert glue.rfind('#if PP_TOUCH_WAKE_ON_BATTERY', 0, i) > glue.rfind('#endif', 0, i), call + ' is behind the macro'

# the gate: inline true with the flag off, the policy state with it on
assert 'static inline bool power_sleep_input_allowed(void) { return true; }' in header
assert re.search(r'bool power_sleep_input_allowed\(void\)\n\{.*?pp_screen_input_allowed\(', glue, re.S)

# the touch task drops a contact before it can wake the screen, and does not stamp activity for it
task = re.search(r'static void task\(void \*arg\)\n\{.*?^\}', touch, re.M | re.S)[0]
assert task.index('!power_sleep_input_allowed()') < task.index('display_wake();')
assert 'if (down && power_sleep_input_allowed())\n            display_bump_activity();' in task

# the ptt task: the PWR tap still toggles the screen (the one wake source), BOOT asks the gate before it can wake or act
assert 'if (pwr_key && power_take_pwrkey_tap()) screen_toggle("PWR tap");' in ptt
press = ptt[ptt.index('int boot_now = gpio_get_level'):]
assert press.index('power_sleep_input_allowed()') < press.index('display_wake()')
print('D1 wake sources: touch arm compiled out by default, the gate is asked by the touch task and the BOOT poll, PWR tap kept PASS')
