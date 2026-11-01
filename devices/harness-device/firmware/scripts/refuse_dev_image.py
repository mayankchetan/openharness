#!/usr/bin/env python3
"""Refuse a firmware image that was built for development: one that accepts an unbonded phone.

    scripts/refuse_dev_image.py <image.bin> [<image.bin> ...]

Exit 0 only when every file is read, is not empty, and does not contain the marker string. A development image (a build made with the BLE_DEV_ALLOW_UNBONDED switch) logs the
marker in the start-up warning of main/ble_link.c; a product image never carries it. upload-firmware.sh and flash-station.sh call this before they publish or
flash. A file that is missing, unreadable or empty is refused too: "I read nothing" must not look like "all clear".
"""
import sys

MARKER = b"BLE_DEV_UNBONDED_IMAGE"
CHUNK = 1 << 20


def contains_marker(path):
    """True if the file holds the marker anywhere, including across a chunk boundary."""
    tail = b""
    total = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            total += len(block)
            if MARKER in tail + block:
                return True, total
            tail = (tail + block)[-(len(MARKER) - 1):]
    return False, total


def main(argv):
    if not argv:
        print("refuse_dev_image: no image given", file=sys.stderr)
        return 2
    bad = 0
    for path in argv:
        try:
            found, total = contains_marker(path)
        except OSError as error:
            print(f"refuse_dev_image: cannot read {path}: {error}", file=sys.stderr)
            bad += 1
            continue
        if total == 0:
            print(f"refuse_dev_image: {path} is empty: not an image", file=sys.stderr)
            bad += 1
        elif found:
            print(f"refuse_dev_image: {path} is a DEVELOPMENT image (it accepts an unbonded phone; marker {MARKER.decode()}). Build without -DBLE_DEV_ALLOW_UNBONDED.",
                  file=sys.stderr)
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
