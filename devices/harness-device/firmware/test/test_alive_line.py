"""The once-a-minute alive line (display_habitat.c heartbeat) carries the battery and still fits the log line limit.

The host watches the prefix "alive up="; cable_link.c cuts a log line at 512 bytes (LOG_LINE_MAX). The field widths below
are the worst case of each conversion.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent / '..'
source = (root / 'main/ui/habitat/display_habitat.c').read_text()
link = (root / 'main/cable_link.c').read_text()

match = re.search(r'ESP_LOGI\("habitat",\s*((?:"[^"]*"\s*)+),', source)
assert match, 'alive line not found'
fmt = ''.join(re.findall(r'"([^"]*)"', match[1]))
assert fmt.startswith('alive up=')
assert ' bat=%d chg=%d mv=%d' in fmt, 'the alive line carries the battery: shown percent, charge state, cell voltage in mV'

limit = int(re.search(r'#define LOG_LINE_MAX (\d+)', link)[1])
widths = {'lu': 10, 'u': 10, 'd': 11}
suffixes = [len(' TOUCH-DEAD'), len(' asleep')]       # the two trailing %s fields
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
assert strings == 2
prefix = len('I (4294967295) habitat: ')
assert worst + prefix <= limit, (worst + prefix, limit)
print(f'alive line: carries bat=, chg= and mv=, worst case {worst + prefix} of {limit} bytes PASS')
