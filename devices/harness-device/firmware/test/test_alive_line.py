"""The once-a-minute alive line (display_habitat.c heartbeat) carries the battery and still fits the log line limit.

The host watches the prefix "alive up="; cable_link.c cuts a log line at 512 bytes (LOG_LINE_MAX). The field widths below
are the worst case of each conversion.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent / '..'
source = (root / 'main/ui/habitat/display_habitat.c').read_text()
link = (root / 'main/cable_link.c').read_text()

match = re.search(r'ESP_LOGI\("habitat",\s*((?:"[^"]*"\s*)+)(PM_ALIVE_FMT)?,', source)
assert match, 'alive line not found'
fmt = ''.join(re.findall(r'"([^"]*)"', match[1]))
assert fmt.startswith('alive up=')
# The battery sleep image adds pm=, slp= and slp_ms= through PM_ALIVE_FMT (empty in every other image): measure the longer line.
pm_fmt = re.search(r'#ifdef DEVICE_BATTERY_SLEEP\n#define PM_ALIVE_FMT "([^"]*)"\n#define PM_ALIVE_ARGS ([^\n]*)\n#else\n#define PM_ALIVE_FMT ""\n#define PM_ALIVE_ARGS\n#endif', source)
assert match[2] and pm_fmt, 'PM_ALIVE_FMT: flag-on and flag-off definitions'
assert pm_fmt[1] == ' pm=%s slp=%lu slp_ms=%lu' and 'power_sleep_state_name()' in pm_fmt[2], 'the alive line carries pm=, slp= and slp_ms= in a battery sleep image'
assert 'display_is_asleep() ? " asleep" : "" PM_ALIVE_ARGS);' in source
fmt_off = fmt
fmt = fmt + pm_fmt[1]
assert ' bat=%d chg=%d mv=%d' in fmt, 'the alive line carries the battery: shown percent, charge state, cell voltage in mV'

limit = int(re.search(r'#define LOG_LINE_MAX (\d+)', link)[1])
widths = {'lu': 10, 'u': 10, 'd': 11}
suffixes = [len(' TOUCH-DEAD'), len(' asleep'), len('S0')]       # the two trailing %s fields, then the pm= state name
worst = 0
strings = 0
for part in re.split(r'(%l?[udsx])', fmt):
    conv = re.fullmatch(r'%(l?[udsx])', part)
    if not conv:
        worst += len(part)
    elif conv[1] == 's':
        worst += suffixes[strings]; strings += 1
    else:
        worst += widths[conv[1]]
assert strings == 3
prefix = len('I (4294967295) habitat: ')
assert worst + prefix <= limit, (worst + prefix, limit)
print(f'alive line: carries bat=, chg= and mv= (and pm=, slp=, slp_ms= in a battery sleep image), worst case {worst + prefix} of {limit} bytes PASS')
