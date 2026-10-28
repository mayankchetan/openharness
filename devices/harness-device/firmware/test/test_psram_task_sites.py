"""Which tasks have a PSRAM stack: the round-2 safe-tier set (ptt, beep, proj_refresh) and no other.

Each moved task creates itself with task_create_psram() under its TASK_STACK_PSRAM_* macro and falls back to
the old plain create under #else. The tasks that touch flash (OTA, NVS saves, SHA over flash) or run a
latency-bound loop stay on internal stacks until their own round and its proof.
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'

MOVED = [  # (file, task name, macro, plain create that the #else branch keeps)
    ('ptt.c', 'ptt', 'TASK_STACK_PSRAM_PTT', 'xTaskCreate'),
    ('audio_capture.c', 'beep', 'TASK_STACK_PSRAM_BEEP', 'xTaskCreate'),
    ('app_main.c', 'proj_refresh', 'TASK_STACK_PSRAM_PROJ', 'xTaskCreate'),
]
STAYS_INTERNAL = ['habitat_render', 'habitat_touch', 'habitat_actions', 'cable_link', 'cable_session',
                  'voice_capture', 'voice_send']

header = (main / 'task_util.h').read_text()
for _, _, macro, _ in MOVED:
    assert re.search(r'#ifndef ' + macro + r'\s+#define ' + macro + r' 1\b', header), macro   # default on

for name, task, macro, plain in MOVED:
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

print('PSRAM task sites: ' + ', '.join(task for _, task, _, _ in MOVED) + ' use task_create_psram under their macros, '
      'the flash-touching and latency-bound tasks keep internal stacks PASS')
