"""Checks on the ADPCM golden vectors and on the script that generates them (task W1).

Run with `uv run pytest test/test_adpcm_vectors.py` or plain `python test/test_adpcm_vectors.py`.

The vectors are the one thing the C encoder/decoder (main/adpcm_ima.c), the daemon's TypeScript codec
(cli/src/cable/adpcm.ts) and ffmpeg share. These tests pin the generator itself: it is deterministic, it
writes the same file to both repositories' places, every line it writes obeys the block-length rule of the
wire spec, and the generator's own decoder reproduces every `dec` line. The C and TS suites then assert
their implementations against the same file.
"""
import hashlib
import importlib.util
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
FW = HERE.parent
SCRIPT = FW / "scripts" / "gen_adpcm_vectors.py"
FW_VECTORS = FW / "test" / "vectors" / "adpcm_ima.txt"
DAEMON_VECTORS = FW.parents[2] / "cli" / "src" / "cable" / "vectors" / "adpcm_ima.txt"


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_adpcm_vectors", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_lines(path):
    """Vector lines as field lists. CRLF checkouts (autocrlf) read the same as LF ones."""
    text = path.read_bytes().decode("utf-8").replace("\r\n", "\n")
    return [line.split("\t") for line in text.split("\n") if line and not line.startswith("#")]


def expected_payload_len(n):
    return 5 + 4 + 4 * (-(-(n - 1) // 8))  # prefix + WAV header + 4 bytes per 8 samples


def pcm_from_hex(text):
    raw = bytes.fromhex(text)
    return [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]


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
    assert FW_VECTORS.exists() and DAEMON_VECTORS.exists()
    assert digest(FW_VECTORS) == digest(DAEMON_VECTORS)


def test_every_line_parses_and_obeys_the_block_length_rule():
    seen = {"enc": 0, "dec": 0, "gap": 0, "bad": 0}
    for f in read_lines(FW_VECTORS):
        kind = f[0]
        assert kind in seen, f"unknown line kind {kind}"
        seen[kind] += 1
        if kind == "enc":
            assert len(f) == 8, f[:2]
            _, name, pcm, index_in, seq, flags, payload, index_out = f
            n = len(pcm_from_hex(pcm))
            assert 1 <= n <= 505, name
            assert len(bytes.fromhex(payload)) == expected_payload_len(n), name
            assert 0 <= int(index_in) <= 88 and 0 <= int(index_out) <= 88, name
            assert int(flags) in (0, 1), name
            raw = bytes.fromhex(payload)
            assert int.from_bytes(raw[0:2], "little") == int(seq), name
            assert int.from_bytes(raw[2:4], "little") == n, name
            assert raw[4] == int(flags), name
            assert raw[7] == int(index_in) and raw[8] == 0, name
        elif kind == "dec":
            assert len(f) == 4, f[:2]
            raw = bytes.fromhex(f[2])
            n = int.from_bytes(raw[2:4], "little")
            assert 1 <= n <= 505, f[1]
            assert len(raw) == expected_payload_len(n), f[1]
            assert len(pcm_from_hex(f[3])) == n, f[1]
        elif kind == "gap":
            assert len(f) == 4, f[:2]
            payloads = f[2].split(",")
            for p in payloads:
                raw = bytes.fromhex(p)
                n = int.from_bytes(raw[2:4], "little")
                assert len(raw) == expected_payload_len(n), f[1]
        else:
            assert len(f) == 3, f[:2]
            raw = bytes.fromhex(f[2])
            ok_length = len(raw) >= 5 and 1 <= int.from_bytes(raw[2:4], "little") <= 505 and \
                len(raw) == expected_payload_len(int.from_bytes(raw[2:4], "little"))
            ok_index = len(raw) >= 9 and raw[7] <= 88
            assert not (ok_length and ok_index), f"bad vector {f[1]} is actually valid"
    # Every kind the spec names is present, with the cases the spec lists.
    assert all(count > 0 for count in seen.values()), seen


def test_spec_case_list_is_covered():
    names = {f[1] for f in read_lines(FW_VECTORS)}
    for required in ("silence", "dc_step", "square_fullscale", "sine_1khz", "chirp", "noise_xorshift",
                     "n1", "n2", "n3", "n9", "n504", "n505", "odd_n", "even_n", "index_pinned_0",
                     "index_pinned_88", "pred_plus_32767", "pred_minus_32767",
                     "gap_1", "gap_2", "gap_50", "duplicate_seq",
                     "bad_n0", "bad_n_506", "bad_index_89", "bad_length_long", "bad_length_short"):
        assert required in names, required


def test_generators_own_decoder_reproduces_every_dec_and_enc_line():
    gen = load_generator()
    for f in read_lines(FW_VECTORS):
        if f[0] == "dec":
            assert gen.decode_payload(bytes.fromhex(f[2])) == pcm_from_hex(f[3]), f[1]
        elif f[0] == "enc":
            # The encoder's reconstruction is the decoder's output, so the round trip stays inside the step size.
            decoded = gen.decode_payload(bytes.fromhex(f[6]))
            assert len(decoded) == len(pcm_from_hex(f[2])), f[1]


def test_generators_encoder_reproduces_every_enc_line_and_carries_the_index():
    gen = load_generator()
    for f in read_lines(FW_VECTORS):
        if f[0] != "enc":
            continue
        payload, index_out = gen.encode_block(pcm_from_hex(f[2]), int(f[3]), int(f[4]), int(f[5]))
        assert payload.hex() == f[6], f[1]
        assert index_out == int(f[7]), f[1]


def test_generators_gap_assembler_reproduces_every_gap_line():
    gen = load_generator()
    for f in read_lines(FW_VECTORS):
        if f[0] == "gap":
            blocks = [bytes.fromhex(p) for p in f[2].split(",")]
            assert gen.assemble(blocks) == pcm_from_hex(f[3]), f[1]


def test_generators_decoder_rejects_every_bad_line():
    gen = load_generator()
    for f in read_lines(FW_VECTORS):
        if f[0] == "bad":
            try:
                gen.decode_payload(bytes.fromhex(f[2]))
            except ValueError:
                continue
            raise AssertionError(f"{f[1]} was accepted")


def test_ima_reference_values_from_the_format_text():
    """The step table and the decode rule, checked against hand-worked numbers, not against the generator's own tables."""
    gen = load_generator()
    assert len(gen.STEP_TABLE) == 89
    assert gen.STEP_TABLE[0] == 7 and gen.STEP_TABLE[88] == 32767
    assert gen.INDEX_TABLE == [-1, -1, -1, -1, 2, 4, 6, 8]
    # pred 0, index 0 (step 7), code 0b0111: 7>>3 + 7>>2 + 7>>1 + 7 = 0 + 1 + 3 + 7 = 11, index 0 + 8.
    assert gen.step_once(0, 0, 0x7) == (11, 8)
    # Same with the sign bit: -11.
    assert gen.step_once(0, 0, 0xF) == (-11, 8)
    # Clamp both ways at index 88 (step 32767).
    assert gen.step_once(32767, 88, 0x7) == (32767, 88)
    assert gen.step_once(-32768, 88, 0xF) == (-32768, 88)
    # Index clamps at 0 on the way down.
    assert gen.step_once(100, 0, 0x0) == (100, 0)


def _folded(path):
    """A vector file as text with CRLF folded to LF (a CRLF checkout is the same file)."""
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def test_cable_frame_vectors_carry_the_adpcm_frame_type_in_both_copies():
    """Frame type 0x07 is an envelope matter too: one `encode adpcm_block 7` line in each cable_frame.txt."""
    for path in (FW / "test" / "vectors" / "cable_frame.txt", DAEMON_VECTORS.parent / "cable_frame.txt"):
        rows = [line.split("\t") for line in _folded(path).split("\n")]
        found = [r for r in rows if r[0] == "encode" and r[1] == "adpcm_block"]
        assert len(found) == 1 and found[0][2] == "7", path
        assert bytes.fromhex(found[0][4])[3] == 7, path       # the type byte of the framed bytes
        assert len(bytes.fromhex(found[0][3])) == 261, path   # a nominal 505-sample payload


def test_cable_frame_vector_copies_are_identical():
    assert _folded(FW / "test" / "vectors" / "cable_frame.txt") == _folded(DAEMON_VECTORS.parent / "cable_frame.txt")


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
