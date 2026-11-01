"""Checks on the BLE link vectors, on the script that generates them, and on the wire spec that names them (task B1).

Run with `python test/test_ble_vectors.py` or `uv run pytest test/test_ble_vectors.py`.

The vector file is the one thing the dial (C), the phone app (Kotlin) and the desktop daemon agree on for the BLE link:
the UUIDs, the PSM, the advertising bytes, the status and hint characteristic values, and the deadlines. These tests pin
the generator (deterministic, writes both copies), the file (every line parses and obeys its own rules), and the spec
(every UUID, constant and state name in the vectors is stated in docs/specs/cable-ble-link.md, and its links resolve).
"""
import hashlib
import importlib.util
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
FW = HERE.parent
REPO = FW.parents[2]
SCRIPT = FW / "scripts" / "gen_ble_vectors.py"
FW_VECTORS = FW / "test" / "vectors" / "ble_status.txt"
PHONE_VECTORS = FW.parent / "phone-bridge" / "src" / "test" / "resources" / "ble_status.txt"
SPEC = REPO / "docs" / "specs" / "cable-ble-link.md"

UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
NAME_RE = re.compile(r"^harness-dial-[0-9a-f]{4}$")
MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_ble_vectors", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def folded(path):
    """File text with CRLF folded: a CRLF checkout is the same file."""
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def read_lines(path):
    return [line.split("\t") for line in folded(path).split("\n") if line and not line.startswith("#")]


def of_kind(kind):
    return [f for f in read_lines(FW_VECTORS) if f[0] == kind]


def ad_structures(raw):
    """Split an advertising payload into (type, data) pairs; fails on a length that overruns the packet."""
    out, i = [], 0
    while i < len(raw):
        n = raw[i]
        assert n >= 1 and i + 1 + n <= len(raw), f"AD length {n} at {i} overruns {len(raw)} bytes"
        out.append((raw[i + 1], raw[i + 2:i + 1 + n]))
        i += 1 + n
    return out


def test_generator_is_deterministic():
    gen = load_generator()
    assert gen.render() == gen.render()
    assert len(gen.render()) > 1000


def test_generator_check_mode_agrees_with_the_files_on_disk():
    result = subprocess.run([sys.executable, str(SCRIPT), "--check"], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_both_vector_copies_are_identical_by_sha256():
    def digest(path):
        return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    assert FW_VECTORS.exists(), FW_VECTORS
    assert PHONE_VECTORS.exists(), PHONE_VECTORS
    assert digest(FW_VECTORS) == digest(PHONE_VECTORS)


def test_every_line_parses_and_obeys_its_rules():
    seen = {"uuid": 0, "psm": 0, "adv": 0, "status": 0, "hint": 0, "const": 0, "ladder": 0}
    uuids = {}
    for f in read_lines(FW_VECTORS):
        kind = f[0]
        assert kind in seen, f"unknown line kind {kind}"
        seen[kind] += 1
        if kind == "uuid":
            assert len(f) == 4, f
            _, role, text, le_hex = f
            assert UUID_RE.match(text), text
            assert bytes.fromhex(le_hex) == bytes.fromhex(text.replace("-", ""))[::-1], role
            uuids[role] = text
        elif kind == "psm":
            assert len(f) == 4, f
            assert int(f[1]) == 0x0080 and f[2] == "0x0080" and f[3] == "8000"   # u16 little endian
        elif kind == "adv":
            assert len(f) == 5, f
            _, mac, name, adv_hex, rsp_hex = f
            assert MAC_RE.match(mac), mac
            assert NAME_RE.match(name), name
            assert name == "harness-dial-" + (mac[-5:-3] + mac[-2:]).lower(), (mac, name)
            adv, rsp = bytes.fromhex(adv_hex), bytes.fromhex(rsp_hex)
            assert len(adv) <= 31 and len(rsp) <= 31, name
            adv_ads = ad_structures(adv)
            assert adv_ads[0] == (0x01, bytes([0x06])), "flags: general discoverable, no BR/EDR"
            assert adv_ads[1] == (0x09, name.encode("ascii")), "complete local name"
            assert len(adv_ads) == 2
            assert ad_structures(rsp) == [(0x07, bytes.fromhex(uuids["service"].replace("-", ""))[::-1])]
        elif kind == "status":
            assert len(f) == 3, f
            assert re.fullmatch(r"[0-9a-f]{2}", f[1]), f
        elif kind == "hint":
            assert len(f) == 3, f
            assert re.fullmatch(r"(|[0-9a-f]{2}([0-9a-f]{2})*)", f[1]), f
            assert f[2] in ("ok:0", "ok:1") or re.fullmatch(r"err:0x[0-9a-f]{2}", f[2]), f
        elif kind == "const":
            assert len(f) == 4, f
            assert re.fullmatch(r"[a-z0-9_]+", f[1]) and f[3] in ("ms", "bytes", "count"), f
            assert int(f[2]) > 0, f
        else:
            assert len(f) == 3 and f[2] == "ms", f
            rungs = [int(x) for x in f[1].split(",")]
            assert rungs == sorted(rungs) and rungs[0] == 1000 and rungs[-1] == 30000, rungs
    assert all(count > 0 for count in seen.values()), seen
    assert set(uuids) == {"service", "psm", "status", "hint"}
    assert len(set(uuids.values())) == 4, "the four UUIDs differ"


def test_status_values_cover_idle_to_no_network_and_unknown_is_not_an_error():
    names = {f[1]: f[2] for f in of_kind("status")}
    assert [names[f"{i:02x}"] for i in range(5)] == [
        "idle", "tunnel_connecting", "tunnel_up", "token_rejected", "phone_no_network"]
    assert names["05"] == "unknown" and names["ff"] == "unknown"


def test_hint_write_rules():
    rules = {f[1]: f[2] for f in of_kind("hint")}
    assert rules["00"] == "ok:0" and rules["01"] == "ok:1"
    assert rules["02"] == "err:0x80" and rules["ff"] == "err:0x80"       # out of range: application error
    assert rules[""] == "err:0x0d" and rules["0101"] == "err:0x0d"       # wrong length: invalid attribute value length


def test_advertising_cases_include_zero_padded_bytes_and_the_bench_dial():
    macs = [f[1] for f in of_kind("adv")]
    assert "B8:1F:3F:CA:E5:58" in macs, "the dial MAC of the 2026-10-28 bench"
    assert any(m[-5:-3] == "00" for m in macs), "a zero first byte of the pair keeps its leading zero"
    assert any(m[-2:] == "00" for m in macs), "a zero second byte of the pair keeps its leading zero"
    assert any(m[-5:-3] != m[-5:-3].upper() or any(c in "ABCDEF" for c in m[-5:]) for m in macs), "hex letters get lower-cased"


def test_spec_exists_and_states_every_uuid_constant_and_state_name():
    assert SPEC.exists(), SPEC
    text = SPEC.read_text(encoding="utf-8")
    for f in of_kind("uuid"):
        assert f[2] in text, f"UUID {f[2]} ({f[1]}) missing from the spec"
    assert "0x0080" in text and "harness-dial-" in text
    lines = text.split("\n")
    for f in of_kind("const"):
        row = [ln for ln in lines if f"`{f[1]}`" in ln]
        assert row, f"constant {f[1]} missing from the spec"
        assert any(re.search(rf"(?<![0-9]){f[2]}(?![0-9])", ln) for ln in row), f"constant {f[1]} = {f[2]} not stated"
    for f in of_kind("status"):
        if f[2] != "unknown":
            assert f"`{f[2]}`" in text, f"status {f[2]} missing from the spec"
    for f in of_kind("ladder"):
        assert f[1].replace(",", ", ") in text or f[1] in text, "backoff ladder missing from the spec"


def test_spec_relative_links_resolve():
    text = SPEC.read_text(encoding="utf-8")
    links = re.findall(r"\]\(([^)#\s]+)(?:#[^)]*)?\)", text)
    for link in links:
        if re.match(r"[a-z]+://", link):
            continue
        assert (SPEC.parent / link).resolve().exists(), f"broken link {link}"
    # The two files the spec points at by path in prose must exist as well.
    assert FW_VECTORS.exists()
    assert "test/vectors/ble_status.txt" in text and "scripts/gen_ble_vectors.py" in text


def test_spec_forbids_the_two_things_the_bench_found_fatal():
    text = SPEC.read_text(encoding="utf-8")
    assert re.search(r"never request(s)? (the )?2M PHY", text, re.I), "FB1 rule: no 2M PHY from the dial"
    assert "never parse" in text.lower(), "relay rule 1: the phone never parses payload bytes"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("ok  ", name)
            except Exception as error:  # noqa: BLE001 - a test runner reports everything
                failures += 1
                print("FAIL", name, "-", type(error).__name__, error)
    sys.exit(1 if failures else 0)
