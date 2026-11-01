#!/usr/bin/env python3
"""Generate the shared BLE link vectors (task B1 of the phone bridge).

    devices/harness-device/firmware/scripts/gen_ble_vectors.py          # writes both copies of ble_status.txt
    devices/harness-device/firmware/scripts/gen_ble_vectors.py --check  # fails if a copy on disk differs

Two copies, one script, nothing hand-copied: the dial's tree (test/vectors/ble_status.txt, read by the C and Python host
tests) and the phone app's tree (devices/harness-device/phone-bridge/src/test/resources/ble_status.txt, read by the
Kotlin JVM tests). The wire spec is docs/specs/cable-ble-link.md; every value here is also written there, and
test/test_ble_vectors.py fails when the two disagree.

Format, tab-separated, `#` comments and blank lines ignored:

    uuid    <role>  <uuid>  <little-endian-bytes-hex>          service, psm, status, hint
    psm     <decimal>  <0xHHHH>  <u16-little-endian-hex>       the value of the PSM characteristic
    adv     <base-mac>  <name>  <adv-data-hex>  <scan-response-hex>
    status  <value-hex>  <state-name>                          the relay-status characteristic (phone writes, dial reads)
    hint    <written-bytes-hex|empty>  <ok:N|err:0xHH>         a write of the home-hint characteristic and its outcome
    const   <name>  <value>  <ms|bytes|count>                  numbers both sides must agree on
    ladder  <comma-separated-ms>  ms                           the WebSocket reconnect back-off (jitter: const ws_backoff_jitter_percent)

This file is generated from the constants below, so a number cannot move on one side only. The checks are the Python
test and the two consumers; nothing here is a measurement.
"""

import argparse
import pathlib
import sys

# One random 128-bit value (uuid4, 2026-11-01), low byte of the first group replaced by the attribute index so the four
# UUIDs sort together in a scanner. Not a registered UUID.
UUID_BASE = "e70301{:02x}-bbe6-4d70-801c-657c8e99501d"
UUIDS = [
    ("service", UUID_BASE.format(0xF0)),
    ("psm", UUID_BASE.format(0xF1)),
    ("status", UUID_BASE.format(0xF2)),
    ("hint", UUID_BASE.format(0xF3)),
]

PSM = 0x0080  # LE credit-based channel, dynamic range 0x0080..0x00FF, fixed on purpose

# Relay-status values: written by the phone app, read (and notified) to the dial. Anything else reads as "unknown" and is
# shown, never an error: a newer phone app must not break an older dial.
STATUS = [
    (0x00, "idle"),
    (0x01, "tunnel_connecting"),
    (0x02, "tunnel_up"),
    (0x03, "token_rejected"),
    (0x04, "phone_no_network"),
]
STATUS_UNKNOWN_SAMPLES = [0x05, 0x7F, 0xFF]

# Home hint: one byte. 0 = the phone does not see the home network, 1 = it does. Rejections are ATT errors.
ATT_INVALID_ATTRIBUTE_VALUE_LENGTH = 0x0D
ATT_APPLICATION_ERROR = 0x80  # start of the application range 0x80..0x9F: "value out of range"

# Dial base MACs for the advertising cases. The first is the dial of the 2026-10-28 bench. The others keep a leading zero
# in each byte of the pair and mix digits with letters, which is where a formatter goes wrong.
ADV_MACS = [
    "B8:1F:3F:CA:E5:58",
    "10:20:30:40:00:0A",
    "10:20:30:40:0A:00",
    "AA:BB:CC:DD:EE:FF",
    "00:00:00:00:00:00",
]

# Numbers both sides must agree on. Sources are the phone-bridge spec sections named in the wire spec.
CONSTS = [
    ("adv_interval_ms", 50, "ms"),            # connectable undirected, legacy advertising
    ("adv_timeout_ms", 30000, "ms"),          # no central for this long: advertising stops, the dial says "Phone not found"
    ("adv_retry_ms", 120000, "ms"),           # after "not found" the dial waits this long, then advertises again (a wake does it at once)
    ("enc_deadline_ms", 3000, "ms"),          # a link not encrypted this long after connect is dropped
    ("dial_tls_deadline_ms", 5000, "ms"),     # dial: TLS handshake after the channel opens, then close the channel
    ("daemon_tls_deadline_ms", 3000, "ms"),   # daemon: from the first byte of the stream
    ("pair_window_ms", 120000, "ms"),         # the Settings "Pair phone" window
    ("dle_request_delay_ms", 1000, "ms"),     # dial asks for DLE 251 this long after connect
    ("dle_octets", 251, "bytes"),
    ("coc_mtu_bytes", 4032, "bytes"),         # the dial's SDU MTU
    ("coc_mps_bytes", 504, "bytes"),          # the dial's MPS
    ("coc_credits", 8, "count"),              # credits the dial grants at connect
    ("write_cap_ms", 2000, "ms"),             # dial: an SDU stalled this long closes the channel
    ("voice_stall_abort_ms", 3000, "ms"),     # a voice turn aborts when the link stalls this long
    ("relay_queue_bytes", 65536, "bytes"),    # phone: bounded queue each way
    ("relay_full_close_ms", 4000, "ms"),      # phone: a queue full this long closes both sides
    ("ws_ping_ms", 25000, "ms"),              # phone: WebSocket ping, keeps the tunnel idle timeout away
    ("max_bonds", 2, "count"),
    ("ws_backoff_jitter_percent", 20, "count"),   # phone: each back-off delay is the ladder step +/- this many percent, uniformly (chosen 2026-11-01; the plan said "with jitter")
]

BACKOFF_LADDER_MS = [1000, 2000, 4000, 8000, 16000, 30000]

FLAGS = bytes([0x02, 0x01, 0x06])  # AD type 0x01, general discoverable + BR/EDR not supported


def uuid_le(text: str) -> bytes:
    """A UUID as it goes on the air: the 16 bytes of the text, last byte first."""
    return bytes.fromhex(text.replace("-", ""))[::-1]


def adv_name(base_mac: str) -> str:
    """`harness-dial-<last two MAC bytes>`: four lower-case hex digits of the BASE MAC (the PSK identity), no separator.

    The BLE address of the dial is the base MAC + 2, so this name matches the cable identity, not the radio address.
    """
    parts = base_mac.split(":")
    assert len(parts) == 6 and all(len(p) == 2 for p in parts), base_mac
    return "harness-dial-" + (parts[4] + parts[5]).lower()


def ad(type_: int, data: bytes) -> bytes:
    assert len(data) + 1 <= 255
    return bytes([len(data) + 1, type_]) + data


def adv_payload(base_mac: str) -> bytes:
    """Advertising packet: flags, then the complete local name (AD type 0x09). 3 + 2 + 17 = 22 bytes of the 31."""
    out = FLAGS + ad(0x09, adv_name(base_mac).encode("ascii"))
    assert len(out) <= 31
    return out


def scan_response() -> bytes:
    """Scan response: the 128-bit service UUID as a complete list (AD type 0x07). 18 bytes of the 31."""
    out = ad(0x07, uuid_le(dict(UUIDS)["service"]))
    assert len(out) <= 31
    return out


def hint_outcome(written: bytes) -> str:
    if len(written) != 1:
        return f"err:0x{ATT_INVALID_ATTRIBUTE_VALUE_LENGTH:02x}"
    if written[0] > 1:
        return f"err:0x{ATT_APPLICATION_ERROR:02x}"
    return f"ok:{written[0]}"


def render() -> str:
    out = [
        "# Shared vectors for the harness BLE link (dial <-> phone app).",
        "# Generated by devices/harness-device/firmware/scripts/gen_ble_vectors.py. Do not hand-edit.",
        "# Wire spec: docs/specs/cable-ble-link.md. Checked by test/test_ble_vectors.py.",
        "#",
        "#   uuid    <role>  <uuid>  <little-endian-bytes-hex>",
        "#   psm     <decimal>  <0xHHHH>  <u16-little-endian-hex>",
        "#   adv     <base-mac>  <name>  <adv-data-hex>  <scan-response-hex>",
        "#   status  <value-hex>  <state-name>",
        "#   hint    <written-bytes-hex|empty>  <ok:N|err:0xHH>",
        "#   const   <name>  <value>  <ms|bytes|count>",
        "#   ladder  <comma-separated-ms>  ms",
        "",
    ]
    for role, text in UUIDS:
        out.append(f"uuid\t{role}\t{text}\t{uuid_le(text).hex()}")
    out.append("")
    out.append(f"psm\t{PSM}\t0x{PSM:04X}\t{PSM.to_bytes(2, 'little').hex()}")
    out.append("")
    for mac in ADV_MACS:
        out.append(f"adv\t{mac}\t{adv_name(mac)}\t{adv_payload(mac).hex()}\t{scan_response().hex()}")
    out.append("")
    for value, name in STATUS:
        out.append(f"status\t{value:02x}\t{name}")
    for value in STATUS_UNKNOWN_SAMPLES:
        out.append(f"status\t{value:02x}\tunknown")
    out.append("")
    for written in (b"\x00", b"\x01", b"\x02", b"\xff", b"", b"\x01\x01"):
        out.append(f"hint\t{written.hex()}\t{hint_outcome(written)}")
    out.append("")
    for name, value, unit in CONSTS:
        out.append(f"const\t{name}\t{value}\t{unit}")
    out.append("")
    out.append("ladder\t" + ",".join(str(x) for x in BACKOFF_LADDER_MS) + "\tms")
    out.append("")
    return "\n".join(out)


def main() -> int:
    assert len(UUIDS) == len({u for _, u in UUIDS}), "the four UUIDs must differ"
    assert adv_name("B8:1F:3F:CA:E5:58") == "harness-dial-e558"

    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if a copy on disk differs from what this run would write")
    args = ap.parse_args()

    firmware = pathlib.Path(__file__).resolve().parent.parent
    paths = [firmware / "test" / "vectors" / "ble_status.txt",
             firmware.parent / "phone-bridge" / "src" / "test" / "resources" / "ble_status.txt"]
    text = render()
    if args.check:
        stale = False
        for path in paths:
            if not path.exists():
                print(f"missing: {path}", file=sys.stderr)
                stale = True
            # folded newlines: a CRLF checkout is the same file.
            elif path.read_bytes().decode("utf-8").replace("\r\n", "\n") != text:
                print(f"stale: {path} - re-run {pathlib.Path(__file__).name}", file=sys.stderr)
                stale = True
        if not stale:
            print("up to date: " + ", ".join(str(p) for p in paths))
        return 1 if stale else 0
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        print(f"wrote {path} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
