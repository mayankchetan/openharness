"""HT_RUNS and HT_CELL_COLS_MAX, right-sized from the UI gallery tour (HT_RIGHT_SIZE, default on in a Focus-only build).

The tour (112 items, both dials, diet2-r1-results.md section 5) used at most 26 runs in one scene (the inbox pages)
and unpacked at most 259 columns of one sprite row (busy_claude_build). The limits are 29 and 272: the measured worst
case plus the three runs of the battery rim (track, fill, percent number), 11 runs (2 x 11 x 248 = 5,456 B of scene) and 208
columns less than before (40 and 480). A run that does not fit is dropped silently by the builders, so ht_metrics_t counts it (runs_dropped, the UIM
line's runs_drop); test_terminal_metrics.c checks the counter and test_uim.c its attribution.

The switch follows HABITAT_FOCUS_ONLY: the companion skins (tim, tux, octopus) are not built in the default image and
draw more than 29 runs (test_octopus asserts the old limit's room), so an all-skins build and the host tests, which
cover every skin, keep 40 and 480.

This test checks the limits against what the source can show without a dial: the built-in sprite frames (every
ht_cell_frame_t initialiser in pets.c and focus.c), the one-byte column field of the pet packs read from the wire, the
generator's own assertion, and that the switch flips between the two sets.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent / '..'
habitat = root / 'main/ui/habitat'

# Measured on the dial with the gallery image (the tour that ran 2026-10-28): the numbers the limits were sized from.
TOUR_RUNS_MAX = 26
TOUR_COLS_MAX = 259


def limits(*flags):
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'limits.c'
        src.write_text('#include <stdio.h>\n#include "terminal.h"\n'
                       'int main(void) { printf("%d %d", (int)HT_RUNS, (int)HT_CELL_COLS_MAX); return 0; }\n')
        exe = Path(tmp) / 'limits'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(habitat), *flags, str(src), '-o', str(exe)], check=True)
        runs, cols = map(int, subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split())
    return runs, cols


FOCUS = '-DHABITAT_FOCUS_ONLY=1'
runs, cols = limits(FOCUS)
assert (runs, cols) == (29, 272), (runs, cols)            # the default image (Focus only): right-sized
assert limits() == (40, 480)                              # every skin built (host tests, DEVICE_ALL_SKINS): old limits
assert limits(FOCUS, '-DHT_RIGHT_SIZE=0') == (40, 480)    # the switch puts the old limits back
assert limits('-DHT_RIGHT_SIZE=1') == (29, 272)           # and forces the new ones
assert runs >= TOUR_RUNS_MAX + 3 and cols >= TOUR_COLS_MAX + 8, 'room over the tour worst case for the battery rim (3 runs)'

# Built-in frames: the widest cell frame in the generated art must fit the unpack row.
widest = 0
for path in sorted(habitat.glob('*.c')):
    for line in path.read_text(errors='replace').splitlines():
        if 'ht_cell_frame_t' in line:
            for match in re.finditer(r'\{(\d+),(\d+),(\d+),', line):
                widest = max(widest, int(match.group(1)))
assert widest == TOUR_COLS_MAX, widest                    # the tour met the widest frame there is
assert widest <= cols

# Pet packs from the wire: the column count is one byte (pet_store.c rd8), so it can never pass the row.
store = (root / 'main/pet_store.c').read_text()
assert re.search(r'_Static_assert\(UINT8_MAX <= HT_CELL_COLS_MAX', store), 'pet_store.c asserts a one-byte cols fits'

# The generator checks the same limit when it packs a frame.
gen = (root / 'scripts/gen_pets.py').read_text()
match = re.search(r'assert cols <= (\d+)\s+# terminal.h HT_CELL_COLS_MAX', gen)
assert match and int(match.group(1)) == cols, 'gen_pets.py limit matches terminal.h'

print(f'render limits: HT_RUNS {runs} (tour worst {TOUR_RUNS_MAX}), HT_CELL_COLS_MAX {cols} (widest frame {widest}) in a Focus-only build, '
      '40/480 with every skin or the switch off, pet pack columns and the generator agree PASS')
