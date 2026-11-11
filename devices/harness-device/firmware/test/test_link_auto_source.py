"""Wifi-first work, T1: source rules for the "Wi-Fi first, else phone" decision module, checked without a toolchain.

  1. link_auto.c and link_auto.h are pure: no IDF, no FreeRTOS, no clock, no random source (the host test links them with a plain compiler).
  2. The constants equal the approved spec (rev 2, 2026-11-11): 30 s window, 2 scans, 120 s Wi-Fi lost, 120 s never connected, 3 retries, edge cap 8,
     600 s stable, drop waits 30 / 60 / 120 s. Each constant is defined once, in the header.
  3. The drop-wait table has exactly LA_RETRY_MAX entries, is read only through la_drop_wait_ms(), and the retries guard comes BEFORE the table is indexed.
  4. An untrusted reset zeroes and re-seals the record (finding 4).
  5. The build and the test run know the module and its tests.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'


def text(path):
    return path.read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    src = re.sub(r'/\*.*?\*/', ' ', text(main / name), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


for name in ('link_auto.c', 'link_auto.h'):
    src = code(name)
    includes = re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', src)
    allowed = {'link_auto.h', 'stdbool.h', 'stdint.h', 'stddef.h', 'string.h'}
    assert set(includes) <= allowed, f'{name} includes {sorted(set(includes) - allowed)}: the module must stay pure'
    for banned in ('esp_', 'freertos', 'esp_timer', 'time(', 'rand(', 'malloc', 'printf', 'ESP_LOG', 'nvs_'):
        assert banned not in src, f'{name} mentions {banned}'

header = code('link_auto.h')
core = code('link_auto.c')

# the numbers of the spec, from the header
want = {
    'LA_TRY_MS': 30000, 'LA_NOAP_SCANS': 2, 'LA_WIFI_LOST_MS': 120000, 'LA_NEVER_MS': 120000,
    'LA_RETRY_MAX': 3, 'LA_EDGE_CAP': 8, 'LA_STABLE_MS': 600000,
}
for name, value in want.items():
    found = re.findall(rf'#define\s+{name}\s+(\d+)u', header)
    assert found == [str(value)], f'{name}: header says {found}, the spec says {value}'
    assert f'#define {name}' not in core, f'{name} is defined once, in the header'
assert re.search(r'#define\s+LA_DROP_WAIT_LIST\s+30000u,\s*60000u,\s*120000u', header), 'drop waits 30 / 60 / 120 s'
# no bare copy of a spec number in the logic
for number in ('30000', '60000', '120000', '600000'):
    assert not re.search(rf'\b{number}u?\b', core), f'link_auto.c holds a bare {number}: use the header constant'

# the table: LA_RETRY_MAX entries, read through la_drop_wait_ms() only
assert 'DROP_WAIT_MS[LA_RETRY_MAX] = {LA_DROP_WAIT_LIST}' in core
assert len(re.findall(r'DROP_WAIT_MS\[', core)) == 2, 'the table is indexed in one place (la_drop_wait_ms) and defined in one'
assert 'retries < LA_RETRY_MAX ? DROP_WAIT_MS[retries] : 0u' in core, 'the table read is guarded'
step = core[core.index('void la_step'):]
guard = step.index('a->retries >= LA_RETRY_MAX')
read = step.index('la_drop_wait_ms(a->retries)')
assert guard < read, 'finding 5: the retries guard comes before the table is read'

# finding 4: an untrusted reset zeroes and re-seals
boot = core[core.index('bool la_boot'):core.index('static la_view_t view_of')]
assert 'if (!ok)' in boot and 'la_seal(r, false, 0, 0)' in boot, 'an untrusted record is zeroed and sealed again'
assert 'reset_is_software &&' in boot, 'only a software reset can make the record trusted'

cmake = text(main / 'CMakeLists.txt')
assert '"link_auto.c"' in cmake, 'link_auto.c is built'
run = text(fw / 'test/run.sh')
for name in ('test_link_auto.c', 'test_link_auto_source.py'):
    assert name in run, name
print('test_link_auto_source: ok')
