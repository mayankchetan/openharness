"""B4e (review M2): a development image (built with BLE_DEV_ALLOW_UNBONDED, which accepts an unbonded phone) must not reach a dial by accident.

Four things hold it:
  1. the image carries a marker string (BLE_DEV_UNBONDED_IMAGE, in the start-up warning of ble_link.c, in a development build only);
  2. scripts/refuse_dev_image.py exits non-zero for any file that contains the marker (it reads in chunks, and finds a marker split across two chunks);
  3. the two scripts that put an image on a dial or in the public bucket, upload-firmware.sh and flash-station.sh, run it on the image BEFORE they upload or flash;
  4. the CMake variable does not persist: the version string gets the suffix +bledev, configure warns, and the cache entry is removed once read (so a later
     build in the same directory without -D is a product build). test_ble_link_source.py pins the CMake side; this file pins the scripts.
The daemon's own firmware offer (cli/src/cable) reads an image from a build directory too; a check there belongs to the daemon track.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
FW = HERE.parent
GUARD = FW / 'scripts' / 'refuse_dev_image.py'
MARKER = b'BLE_DEV_UNBONDED_IMAGE'

assert GUARD.exists(), 'scripts/refuse_dev_image.py is missing'


def run(*paths):
    return subprocess.run([sys.executable, str(GUARD), *map(str, paths)], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    clean = tmp / 'product.bin'
    clean.write_bytes(bytes(range(256)) * 4096 + b'DEVELOPMENT BUILD but not the marker' + b'\x00' * 100)
    dev = tmp / 'dev.bin'
    dev.write_bytes(b'\xff' * 5000 + b'I (5) ble_link: DEVELOPMENT BUILD ' + MARKER + b': an unbonded phone is accepted\x00' + b'\xff' * 5000)

    assert run(clean).returncode == 0, 'a product image passes'
    refused = run(dev)
    assert refused.returncode != 0 and 'dev' in (refused.stdout + refused.stderr).lower(), 'a development image is refused, and the message says why'
    assert str(dev) in refused.stdout + refused.stderr, 'the message names the file'
    # any one bad file among several is enough
    assert run(clean, dev).returncode != 0 and run(dev, clean).returncode != 0
    # the marker at the very start, the very end, and split across the read boundary of the guard
    for name, blob in (('head', MARKER + b'\x00' * 10), ('tail', b'\x00' * 10 + MARKER), ('split', b'\x00' * ((1 << 20) - 5) + MARKER + b'\x00' * 9)):
        f = tmp / f'{name}.bin'
        f.write_bytes(blob)
        assert run(f).returncode != 0, f'marker at the {name} of a file is found'
    # a file that is missing, or no argument at all, is an error: the guard never says "fine" because it read nothing
    assert run(tmp / 'nothing.bin').returncode != 0
    assert run().returncode != 0
    # an empty file holds no marker, but it is not an image either: refuse it, so a failed build cannot be flashed as "clean"
    empty = tmp / 'empty.bin'
    empty.write_bytes(b'')
    assert run(empty).returncode != 0

# the marker in the guard is the one in the firmware source, and only there
link = (FW / 'main' / 'ble_link.c').read_text(encoding='utf-8')
assert MARKER.decode() in link and link.count(MARKER.decode()) == 1
assert MARKER.decode() in GUARD.read_text(encoding='utf-8')

# the two scripts run it on the image before the image leaves
upload = (FW / 'scripts' / 'upload-firmware.sh').read_text(encoding='utf-8').replace('\r\n', '\n')
call = 'python3 "$HERE/scripts/refuse_dev_image.py" "$BIN"'
assert call in upload, 'upload-firmware.sh checks the image'
assert upload.index(call) < upload.index('gcloud storage cp --cache-control'), 'before the upload'
assert upload.index(call) > upload.index('[ -f "$BIN" ] || '), 'after the image is known to exist'
flash = (FW / 'scripts' / 'flash-station.sh').read_text(encoding='utf-8').replace('\r\n', '\n')
fcall = 'python3 "$HERE/scripts/refuse_dev_image.py" "$IMAGE"'
assert fcall in flash, 'flash-station.sh checks the image'
assert flash.index(fcall) > flash.index('[ -f "$IMAGE" ] || '), 'after the image exists'
assert flash.index(fcall) < flash.index('[ "$MERGE_ONLY" = "1" ] && exit 0'), 'before anything is flashed, merge-only or not'
assert flash.index(fcall) < flash.index('flash_one()'), 'before the flash loop'
assert 'HERE=' in flash.split(fcall)[0], 'HERE is set before the call'

print('dev image guard: the marker is in a development image only, the guard refuses it (and a missing or empty file), upload and flash scripts run it before they act PASS')
