"""HT_RENDER_DATA_PSRAM (plan C3a + C3b): the two scene buffers and the two arc caches live in PSRAM.

scenes[2] (display_habitat.c, 19,848 B) and arc_caches[2] (terminal.c, 21,824 B) are written by the renderer only
and read by the CPU only, never by DMA. Measured on the dial (diet2-r1-results.md section 7, S1 vs S0): +40,920 B
internal RAM free, full-frame max +0.1 percent, +0.9 percent under a PSRAM copy load of 17.8 MB/s.

The placement is `HT_RENDER_BSS` (terminal.h): EXT_RAM_BSS_ATTR when the IDF build is on (ESP_PLATFORM) and the
switch is 1, nothing otherwise, so every host test that compiles terminal.c keeps ordinary BSS. This test preprocesses
terminal.c against a fake esp_attr.h and reads the declaration back; display_habitat.c cannot be preprocessed
without the IDF headers, so its declaration is read from the source.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent / '..'
habitat = root / 'main/ui/habitat'

header = (habitat / 'terminal.h').read_text()
assert re.search(r'#ifndef HT_RENDER_DATA_PSRAM\s+#define HT_RENDER_DATA_PSRAM 1\b', header), 'default is on'
assert re.search(r'defined\(ESP_PLATFORM\)', header), 'host builds never reach esp_attr.h'

display = (habitat / 'display_habitat.c').read_text()
assert re.search(r'^static HT_RENDER_BSS ht_scene_t scenes\[2\];', display, re.M), 'scenes[2] placement'

cmake = (root / 'main/CMakeLists.txt').read_text()
assert 'HT_RENDER_DATA_PSRAM' in cmake, 'CMake passes the switch through'

FAKE_ATTR = '#define EXT_RAM_BSS_ATTR __attribute__((section(".ext_ram.bss")))\n'


def declaration(*flags):
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / 'esp_attr.h').write_text(FAKE_ATTR)
        text = subprocess.run(['cc', '-E', '-P', '-std=c11', '-I', tmp, '-I', str(habitat), *flags, str(habitat / 'terminal.c')],
                              check=True, capture_output=True, text=True).stdout
    found = re.findall(r'^static [^;]*\barc_caches\[2\];', text, re.M)
    assert len(found) == 1, found
    return found[0]


on = declaration('-DESP_PLATFORM')
assert '.ext_ram.bss' in on, on                                  # board build, default: PSRAM
assert '.ext_ram.bss' not in declaration('-DESP_PLATFORM', '-DHT_RENDER_DATA_PSRAM=0')   # switch off: internal BSS
assert '.ext_ram.bss' not in declaration()                       # host build: ordinary BSS, no esp_attr.h needed

print('HT_RENDER_DATA_PSRAM: arc_caches[2] and scenes[2] use HT_RENDER_BSS, on by default, off by the switch and on the host PASS')
