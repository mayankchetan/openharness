"""Which tasks have a PSRAM stack: ptt, beep, proj_refresh (round 2) and habitat_actions, cable_session, voice_send
(round 3, TASK_STACK_PSRAM_FLASH_OPS), and no other.

Each moved task creates itself with task_create_psram() under its TASK_STACK_PSRAM_* macro and falls back to
the old plain create under #else. cable_link joins them only when the OTA path is compiled out (esp_ota_* maps flash,
which asserts on a PSRAM stack, diet2-r1-results.md section 1); the render and touch tasks and voice_capture are latency-bound: those stay on
internal stacks.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'

MOVED = [  # (file, task name, macro, plain create that the #else branch keeps)
    ('ptt.c', 'ptt', 'TASK_STACK_PSRAM_PTT', 'xTaskCreate'),
    ('audio_capture.c', 'beep', 'TASK_STACK_PSRAM_BEEP', 'xTaskCreate'),
    ('app_main.c', 'proj_refresh', 'TASK_STACK_PSRAM_PROJ', 'xTaskCreate'),
    ('ui/habitat/ui_habitat.c', 'habitat_actions', 'TASK_STACK_PSRAM_FLASH_OPS', 'xTaskCreate'),
    ('cable_client.c', 'cable_session', 'TASK_STACK_PSRAM_FLASH_OPS', 'xTaskCreate'),
    ('audio_client_habitat.c', 'voice_send', 'TASK_STACK_PSRAM_FLASH_OPS', 'xTaskCreatePinnedToCore'),
    # Wi-Fi mode (W11): the connect / TLS / socket / backoff task. Its call graph holds no flash operation (test_net_link_source.py walks it);
    # the Wi-Fi bring-up, which may write the PHY calibration to NVS, runs on net_init, which is internal and short-lived.
    ('net_link.c', 'net_link', 'TASK_STACK_PSRAM_NET', 'xTaskCreate'),
]
# mode_sw (W15, mode_glue.c): writes NVS (boot_usb) and installs and removes the USB driver, so its stack is internal
STAYS_INTERNAL = ['habitat_render', 'habitat_touch', 'voice_capture', 'net_init', 'net_store', 'mode_sw']

header = (main / 'task_util.h').read_text()
# The cable_link reader is the one conditional task: in PSRAM only with the OTA path compiled out (DEVICE_NO_OTA), because
# esp_ota_begin asserts on a PSRAM stack. Its macro follows the switch and is an #error without it (test_no_ota.py).
READER = ('cable_link.c', 'cable_link', 'TASK_STACK_PSRAM_READER', 'xTaskCreate')
assert re.search(r'#ifndef TASK_STACK_PSRAM_READER\s+#ifdef DEVICE_NO_OTA\s+#define TASK_STACK_PSRAM_READER 1', header)
for macro in sorted({m for _, _, m, _ in MOVED}):
    assert re.search(r'#ifndef ' + macro + r'\s+#define ' + macro + r' 1\b', header), macro   # default on

for name, task, macro, plain in MOVED + [READER]:
    source = (main / name).read_text()
    assert '#include "task_util.h"' in source, name
    block = re.search(r'#if ' + macro + r'\s(.*?)#else\s(.*?)#endif', source, re.S)
    assert block, f'{name}: no #if {macro} ... #else ... #endif'
    assert 'task_create_psram(' in block[1] and f'"{task}"' in block[1], f'{name}: PSRAM branch'
    assert re.search(r'\b' + plain + r'\(', block[2]) and f'"{task}"' in block[2], f'{name}: internal branch'

for path in main.rglob('*.c'):
    for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        if any(f'"{task}"' in line for task in STAYS_INTERNAL):
            assert 'task_create_psram' not in line and 'WithCaps' not in line, f'{path.name}:{number} moved too early'

# net_init is the internal half of Wi-Fi mode: created with a plain xTaskCreate, never deleted-with-caps, and it is the one place nwifi_start() runs.
net_c = (main / 'net_link.c').read_text()
assert re.search(r'xTaskCreate\(net_init_task, "net_init", NET_INIT_STACK', net_c), 'net_init has an internal stack'
assert 'net_init' not in ''.join(re.findall(r'task_create_psram\([^;]*;', net_c))

print('PSRAM task sites: ' + ', '.join(task for _, task, _, _ in MOVED + [READER]) + ' use task_create_psram under their macros '
      '(cable_link only with OTA compiled out), the latency-bound tasks keep internal stacks PASS')
