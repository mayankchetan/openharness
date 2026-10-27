#!/usr/bin/env python3
"""Host side of the dial debug commands (see docs/DEBUG_COMMANDS.md).

  dial_debug.py COM8 shot out.png
  dial_debug.py COM8 tap 233 233
  dial_debug.py COM8 swipe 380 233 90 233 [ms]
  dial_debug.py COM8 press 233 233 [ms]

Needs pyserial. Opens the port with DTR/RTS low so the open does not reset the board.
"""
import json
import struct
import sys
import time
import zlib

import serial

MAGIC = b"\xa5\x48"
VER = 1
T_JSON, T_SHOT = 0x01, 0x06


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype, payload):
    body = bytes([VER, ftype]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class Reader:
    """Pulls frames out of the byte stream, stepping over bare console text."""

    def __init__(self, port):
        self.port = port
        self.buf = bytearray()

    def frames(self, deadline):
        while time.time() < deadline:
            self.buf += self.port.read(4096)
            while True:
                i = self.buf.find(MAGIC)
                if i < 0:
                    del self.buf[:-1]
                    break
                del self.buf[:i]
                if len(self.buf) < 6:
                    break
                n = struct.unpack_from("<H", self.buf, 4)[0]
                total = 6 + n + 2
                if n > 8192:
                    del self.buf[:1]
                    continue
                if len(self.buf) < total:
                    break
                want = struct.unpack_from("<H", self.buf, total - 2)[0]
                if crc16(bytes(self.buf[2:total - 2])) != want:
                    del self.buf[:1]
                    continue
                ftype, payload = self.buf[3], bytes(self.buf[6:6 + n])
                del self.buf[:total]
                yield ftype, payload


def send_json(port, obj):
    port.write(encode(T_JSON, json.dumps(obj, separators=(",", ":")).encode()))
    port.flush()


def write_png(path, w, h, rgb565be):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = rgb565be[y * w * 2:(y + 1) * w * 2]
        for x in range(w):
            v = (row[2 * x] << 8) | row[2 * x + 1]
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            raw += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def cmd_shot(port, out, timeout=20):
    rd = Reader(port)
    rid = int(time.time()) & 0xFFFF
    send_json(port, {"t": "debug.shot", "id": rid})
    begin, chunks = None, {}
    for ftype, payload in rd.frames(time.time() + timeout):
        if ftype == T_JSON:
            msg = json.loads(payload)
            if msg.get("id") != rid:
                continue
            t = msg.get("t")
            if t == "debug.error":
                sys.exit("device refused: " + msg.get("reason", "?"))
            if t == "debug.shot.begin":
                begin = msg
            elif t == "debug.shot.end":
                if begin is None:
                    sys.exit("end without begin")
                missing = [i for i in range(begin["chunks"]) if i not in chunks]
                if missing:
                    sys.exit("lost chunks: %d (first %d)" % (len(missing), missing[0]))
                pix = b"".join(chunks[i] for i in range(begin["chunks"]))
                if len(pix) != begin["bytes"]:
                    sys.exit("size mismatch %d vs %d" % (len(pix), begin["bytes"]))
                got = zlib.crc32(pix) & 0xFFFFFFFF
                if got != msg["crc32"]:
                    sys.exit("crc32 mismatch %08x vs %08x" % (got, msg["crc32"]))
                write_png(out, begin["w"], begin["h"], pix)
                print("saved %s (%dx%d, crc32 %08x ok)" % (out, begin["w"], begin["h"], got))
                return
        elif ftype == T_SHOT and len(payload) >= 4:
            sid, idx = struct.unpack_from("<HH", payload)
            if sid == rid:
                chunks[idx] = payload[4:]
    sys.exit("timeout: got %d chunks" % len(chunks))


def cmd_touch(port, obj, timeout=8):
    rd = Reader(port)
    obj["id"] = int(time.time()) & 0xFFFF
    send_json(port, obj)
    for ftype, payload in rd.frames(time.time() + timeout):
        if ftype != T_JSON:
            continue
        msg = json.loads(payload)
        if msg.get("id") == obj["id"] and msg.get("t") in ("debug.done", "debug.error"):
            print(json.dumps(msg))
            sys.exit(0 if msg["t"] == "debug.done" else 1)
    sys.exit("timeout waiting for reply")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    name, verb, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = name, 115200, 0.2
    port.dtr = False
    port.rts = False
    port.open()
    try:
        if verb == "shot":
            cmd_shot(port, args[0])
        elif verb == "tap":
            cmd_touch(port, {"t": "debug.tap", "x": int(args[0]), "y": int(args[1])})
        elif verb == "press":
            ms = int(args[2]) if len(args) > 2 else 700
            cmd_touch(port, {"t": "debug.press", "x": int(args[0]), "y": int(args[1]), "ms": ms})
        elif verb == "swipe":
            ms = int(args[4]) if len(args) > 4 else 250
            cmd_touch(port, {"t": "debug.swipe", "x1": int(args[0]), "y1": int(args[1]),
                             "x2": int(args[2]), "y2": int(args[3]), "ms": ms})
        else:
            sys.exit(__doc__)
    finally:
        port.close()


if __name__ == "__main__":
    main()
