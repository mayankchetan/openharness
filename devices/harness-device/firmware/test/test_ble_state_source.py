"""B4a: source rules for the BLE state module, checked without a toolchain.

  1. ble_state.c and ble_state.h are pure: no IDF, no NimBLE, no FreeRTOS, no clock, no random source (the host test links them with a plain compiler).
  2. The refusal run reuses the Wi-Fi fold (net_handshake_event), not a second copy of the three-alerts rule.
  3. The build and the test run know the module and its tests.
"""
from pathlib import Path
import re

fw = Path(__file__).resolve().parent / '..'
main = fw / 'main'


def text(name):
    return (main / name).read_text(encoding='utf-8').replace('\r\n', '\n')


def code(name):
    src = re.sub(r'/\*.*?\*/', ' ', text(name), flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


for name in ('ble_state.c', 'ble_state.h'):
    src = code(name)
    includes = re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', src)
    allowed = {'ble_state.h', 'net_state.h', 'stdbool.h', 'stdint.h', 'stddef.h', 'string.h'}
    assert set(includes) <= allowed, f'{name} includes {sorted(set(includes) - allowed)}: the module must stay pure'
    for banned in ('esp_', 'freertos', 'nimble', 'ble_gap', 'ble_l2cap', 'ble_hs', 'esp_timer', 'time(', 'rand(', 'malloc', 'printf', 'ESP_LOG'):
        assert banned not in src, f'{name} mentions {banned}'

core = code('ble_state.c')
assert 'net_handshake_event(&b->refusals, refused)' in core and 'net_refusal_reset(&b->refusals)' in core, 'the refusal run is net_state.c\'s'
assert 'NEXT[BLE_ST_COUNT][BLE_EV_COUNT]' in core

cmake = text('CMakeLists.txt')
assert '"ble_state.c"' in cmake, 'ble_state.c is built'
run = (fw / 'test/run.sh').read_text().replace('\r\n', '\n')
for name in ('test_ble_state.c', 'test_ble_state_source.py'):
    assert name in run, name

print('ble state: pure module, the refusal run is the Wi-Fi fold, built and tested PASS')
