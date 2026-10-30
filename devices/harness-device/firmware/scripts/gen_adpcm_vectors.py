#!/usr/bin/env python3
"""Generate the shared golden vectors for the dial's ADPCM voice blocks (cable frame type 0x07).

    devices/harness-device/firmware/scripts/gen_adpcm_vectors.py         # writes both copies
    devices/harness-device/firmware/scripts/gen_adpcm_vectors.py --check # fails if a copy on disk differs

A THIRD implementation, written from the block description in docs/specs/cable-wifi-link.md (section
"ADPCM block format", which is wifi-mode spec section 7.2) and from nothing else. The firmware encoder and
decoder (main/adpcm_ima.c) and the daemon's codec (cli/src/cable/adpcm.ts) each assert against the file this
writes, so a misreading shared by both halves shows up as a disagreement with the vectors instead of being
blessed by two implementations that copied each other. Nothing here imports, includes or reads either one.

The two halves live in different directories and cannot import a line of code between them, so the file is
written to BOTH places by this script (nothing is hand-copied):

    devices/harness-device/firmware/test/vectors/adpcm_ima.txt
    cli/src/cable/vectors/adpcm_ima.txt

Format, tab-separated, `#` comments and blank lines ignored, PCM as little-endian int16 hex:

    enc  <name>  <pcm-hex>  <index-in>  <seq>  <flags>  <payload-hex>  <index-out>  <tag>
    dec  <name>  <payload-hex>  <pcm-hex>
    gap  <name>  <payload-hex,payload-hex,...>  <pcm-hex>  <turn-tag or ->
    bad  <name>  <payload-hex>

`enc`: the encoder given the samples, the step index to start from and the turn tag must produce exactly
`payload`, and end on `index-out` (the index the NEXT block starts from; the encoder carries it). The tag is
the reserved byte of the WAV block (payload offset 8). `dec`: a payload any valid sender could have
produced, and the samples a decoder must return (exactly `n` of them). `gap`: the blocks that ARRIVED, in
arrival order, and the PCM the daemon must hand to the transcriber, for a turn whose tag is `turn-tag` (`-`
= the turn named none, so no tag is checked): a block whose tag is not the turn's is dropped; a block whose
seq is not newer than the last is dropped; a block more than MAX_GAP_BLOCKS (32) past the next expected seq
is dropped WITHOUT moving the next expected seq; otherwise a missing sequence number becomes 505 zero
samples per missing block. `bad`: a payload a decoder must reject without a crash and without reading past
the buffer.
"""

import argparse
import pathlib
import sys

PREFIX_BYTES = 5
BLOCK_SAMPLES = 505          # (256 - 4) * 2 + 1: a full WAV IMA mono block of 256 bytes
MAX_INDEX = 88
MAX_GAP_BLOCKS = 32          # the most blocks one gap may fill (about 1 s); a block further ahead is refused

# The standard IMA/DVI step table (89 entries) and index adjustment table.
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8]
assert len(STEP_TABLE) == 89


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


def step_once(pred: int, index: int, code: int):
    """One nibble: diff = step>>3 (+step>>2 if b0)(+step>>1 if b1)(+step if b2), negate if b3, clamp to int16.

    Returns (new predictor, new index) with index += INDEX_TABLE[code & 7] clamped to 0..88.
    """
    step = STEP_TABLE[index]
    diff = step >> 3
    if code & 1:
        diff += step >> 2
    if code & 2:
        diff += step >> 1
    if code & 4:
        diff += step
    if code & 8:
        diff = -diff
    return clamp(pred + diff, -32768, 32767), clamp(index + INDEX_TABLE[code & 7], 0, MAX_INDEX)


def payload_len(n: int) -> int:
    """5-byte prefix + 4-byte WAV header + 4 bytes for every 8 samples after the first."""
    return PREFIX_BYTES + 4 + 4 * (-(-(n - 1) // 8))


def encode_block(pcm, index_in: int, seq: int, flags: int, tag: int = 0):
    """Encode 1..505 samples. Returns (payload bytes, step index to start the next block from).

    `tag` is the turn tag, written to the reserved byte of the WAV block (payload offset 8).

    The first sample is written verbatim as the predictor, so it is lossless; the others are quantised
    by the classic IMA rule (sign, then three successive compares against step, step/2, step/4) against
    the predictor the DECODER will hold, which `step_once` tracks.
    """
    n = len(pcm)
    assert 1 <= n <= BLOCK_SAMPLES and 0 <= index_in <= MAX_INDEX and 0 <= seq <= 0xFFFF and 0 <= flags <= 0xFF
    assert 0 <= tag <= 0xFF
    pred, index = pcm[0], index_in
    codes = []
    for sample in pcm[1:]:
        step = STEP_TABLE[index]
        diff = sample - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        pred, index = step_once(pred, index, code)
        codes.append(code)
    while len(codes) % 8:
        codes.append(0)                       # zero nibbles pad a short block up to a multiple of 8
    out = bytearray()
    out += seq.to_bytes(2, "little") + n.to_bytes(2, "little") + bytes([flags])
    out += (pcm[0] & 0xFFFF).to_bytes(2, "little") + bytes([index_in, tag])
    for i in range(0, len(codes), 2):
        out.append(codes[i] | (codes[i + 1] << 4))   # low nibble first
    assert len(out) == payload_len(n)
    return bytes(out), index


def decode_payload(payload: bytes):
    """The samples of one type-0x07 payload, exactly `n` of them. ValueError when a decoder must reject it."""
    if len(payload) < PREFIX_BYTES:
        raise ValueError("shorter than the prefix")
    n = int.from_bytes(payload[2:4], "little")
    if n < 1 or n > BLOCK_SAMPLES:
        raise ValueError(f"n {n} outside 1..{BLOCK_SAMPLES}")
    if len(payload) != payload_len(n):
        raise ValueError(f"length {len(payload)} is not {payload_len(n)} for n={n}")
    index = payload[7]
    if index > MAX_INDEX:
        raise ValueError(f"step index {index} above {MAX_INDEX}")
    pred = int.from_bytes(payload[5:7], "little", signed=True)
    samples = [pred]
    for byte in payload[9:]:
        for code in (byte & 15, byte >> 4):
            pred, index = step_once(pred, index, code)
            samples.append(pred)
    return samples[:n]


def assemble(blocks, turn_tag=None):
    """What the daemon hands the transcriber for the blocks that arrived (in arrival order).

    `turn_tag` None: the turn named no tag and every tag is taken. Otherwise a block whose reserved byte differs is
    dropped (a late block of the turn before). A block too far ahead is dropped too, and `next_seq` does not move.
    """
    pcm, next_seq = [], 0
    for payload in blocks:
        try:
            samples = decode_payload(payload)
        except ValueError:
            continue
        if turn_tag is not None and payload[8] != turn_tag:
            continue                          # not this turn's block
        seq = int.from_bytes(payload[0:2], "little")
        if seq < next_seq:
            continue                          # a duplicate or an older block
        if seq - next_seq > MAX_GAP_BLOCKS:
            continue                          # a forged or damaged seq: refused, and the real blocks after it still count
        pcm += [0] * ((seq - next_seq) * BLOCK_SAMPLES)
        pcm += samples
        next_seq = seq + 1
    return pcm


# ---- signals: integer arithmetic only, so every platform writes the same bytes ---------------------------

SINE_16 = [0, 4592, 8485, 11087, 12000, 11087, 8485, 4592, 0, -4592, -8485, -11087, -12000, -11087, -8485, -4592]
# A 1 kHz sine at 16 kHz repeats every 16 samples; 12000 * sin(k * 22.5 degrees), rounded, written out.


def sine(n: int, offset: int = 0):
    return [SINE_16[(i + offset) % 16] for i in range(n)]


def chirp(n: int):
    """A triangle-wave sweep from 100 Hz to 4 kHz at 16 kHz, integer phase accumulator."""
    inc0, inc1 = (100 << 32) // 16000, (4000 << 32) // 16000
    phase, out = 0, []
    for i in range(n):
        phase = (phase + inc0 + (inc1 - inc0) * i // max(1, n - 1)) & 0xFFFFFFFF
        p = phase >> 16
        tri = p if p < 32768 else 65535 - p
        out.append((tri - 16384) * 20000 // 16384)
    return out


class XorShift:
    """The generator of main/../test/test_cable_transport.c (xorshift32, same seed)."""

    def __init__(self, seed: int = 0xD00E5193):
        self.state = seed

    def word(self) -> int:
        s = self.state
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.state = s
        return s


def noise(n: int, shift: int = 0):
    rng, out = XorShift(), []
    for _ in range(n):
        value = rng.word() >> 16
        value -= 65536 if value >= 32768 else 0
        out.append(value >> shift)
    return out


def chain(pcm, index0: int, first_seq: int = 0, tag: int = 0):
    """Split a long signal into 505-sample blocks, carrying the step index; the last block is flagged."""
    blocks, index, seq = [], index0, first_seq
    pieces = [pcm[i:i + BLOCK_SAMPLES] for i in range(0, len(pcm), BLOCK_SAMPLES)]
    for i, piece in enumerate(pieces):
        payload, nxt = encode_block(piece, index, seq, 1 if i == len(pieces) - 1 else 0, tag)
        blocks.append((piece, index, seq, 1 if i == len(pieces) - 1 else 0, payload, nxt))
        index, seq = nxt, seq + 1
    return blocks


def enc_cases():
    """(name, samples, index-in, seq, flags, tag)."""
    cases = [
        ("silence", [0] * 505, 0, 0, 0, 0),
        ("dc_step", [0] * 252 + [8000] * 253, 0, 1, 0, 1),
        ("dc_step_idx40", [0] * 252 + [8000] * 253, 40, 2, 0, 2),
        # Full-scale square: clips the predictor both ways and drives the index to its ceiling.
        ("square_fullscale", ([32767] * 20 + [-32767] * 20) * 12 + [32767] * 25, 0, 3, 0, 255),
        ("sine_1khz", sine(505), 0, 4, 0, 3),
        ("sine_1khz_idx20", sine(505), 20, 5, 0, 0),
        ("chirp", chirp(505), 0, 6, 0, 128),
        ("noise_xorshift", noise(505), 0, 7, 0, 90),
        ("noise_xorshift_small", noise(505, 6), 12, 8, 0, 0),
        # n = 1 has no nibbles at all; 2 and 3 pad 7 and 6 zero nibbles; 9 fills exactly one group of 8.
        ("n1", [1234], 5, 9, 1, 7),
        ("n2", [-1234, 4321], 5, 10, 1, 0),
        ("n3", sine(3, 3), 30, 11, 1, 165),
        ("n9", sine(9), 0, 12, 1, 0),
        ("n17", chirp(17), 9, 13, 1, 1),
        ("n504", sine(504, 5), 0, 14, 0, 0),
        ("n505", chirp(505), 44, 15, 0, 7),
        # An odd and an even count whose (n - 1) is not a multiple of 8: the zero-nibble padding.
        ("odd_n", sine(101, 2), 18, 16, 1, 254),
        ("even_n", sine(100, 7), 18, 17, 1, 0),
        # The index sits on its floor or its ceiling for the whole block.
        ("index_pinned_0", [0, 1, 0, -1] * 8 + [0], 0, 18, 0, 0),
        ("index_pinned_88", [32767, -32767] * 252 + [32767], 88, 19, 0, 16),
        # The predictor starts, and stays, at the ends of the int16 range.
        ("pred_plus_32767", [32767] * 505, 40, 20, 0, 0),
        ("pred_minus_32767", [-32767] * 505, 40, 21, 0, 51),
        ("pred_minus_32768", [-32768] * 505, 40, 22, 0, 0),
        ("pred_plus_32767_then_swing", [32767] + [-32768] * 40, 0, 23, 0, 195),
    ]
    # Three consecutive blocks of one signal: each starts from the index the one before ended on.
    for i, (piece, index_in, seq, flags, _payload, _nxt) in enumerate(chain(chirp(1200), 3, 100, 9)):
        cases.append((f"chirp_chain_{i}", piece, index_in, seq, flags, 9))
    return cases


def all_codes_payload(pred: int, index: int, n: int = 65, *, flags: int = 0, tag: int = 0, seq: int = 0, fill=None):
    """A payload built by hand: nibbles 0,1,2,..,15 repeating (or `fill`), for the decoder-only cases."""
    codes = [(fill if fill is not None else i % 16) for i in range(n - 1)]
    pads = (-len(codes)) % 8
    codes += [0xA] * pads if fill is None and n < 20 else [0] * pads   # non-zero padding must be ignored
    out = bytearray(seq.to_bytes(2, "little") + n.to_bytes(2, "little") + bytes([flags]))
    out += (pred & 0xFFFF).to_bytes(2, "little") + bytes([index, tag])
    for i in range(0, len(codes), 2):
        out.append(codes[i] | (codes[i + 1] << 4))
    return bytes(out)


def dec_cases():
    """(name, payload) for payloads a decoder must accept, beyond what the encoder produces."""
    return [
        ("all_codes_p0_i0", all_codes_payload(0, 0)),
        ("all_codes_p0_i44", all_codes_payload(0, 44)),
        ("all_codes_pmax_i88", all_codes_payload(32767, 88)),
        ("all_codes_pmin_i88", all_codes_payload(-32768, 88)),
        ("all_codes_p100_i0", all_codes_payload(100, 0)),
        ("code_7_run_p0_i0", all_codes_payload(0, 0, 65, fill=7)),
        ("code_15_run_p0_i0", all_codes_payload(0, 0, 65, fill=15)),
        ("code_0_run_i88", all_codes_payload(5, 88, 65, fill=0)),
        # The tag byte (any value) and the high flag bits are not ours to reject; padding nibbles are never decoded into the output.
        ("tag_byte_ff", all_codes_payload(-5, 20, 33, tag=0xFF)),
        ("tag_byte_a5", all_codes_payload(-5, 20, 33, tag=0xA5)),
        ("flags_high_bits", all_codes_payload(77, 20, 33, flags=0xFE)),
        ("pad_nibbles_nonzero", all_codes_payload(9, 3, 10)),
        ("last_flag_short", all_codes_payload(-300, 60, 2, flags=1)),
        ("full_block_505", all_codes_payload(1, 1, 505)),
    ]


def gap_cases():
    """(name, turn tag or None, [payload...]) in arrival order."""
    signal = sine(505 * 4 + 100)
    blocks = chain(signal, 0)                 # five blocks, seq 0..4, the last short and flagged, all tagged 0

    def block(i, seq=None, tag=0):
        piece, index_in, own_seq, flags, payload, _ = blocks[i]
        if (seq is None or seq == own_seq) and tag == 0:
            return payload
        return encode_block(piece, index_in, own_seq if seq is None else seq, flags, tag)[0]

    return [
        ("gap_1", None, [block(0), block(2, 2), block(3, 3)]),
        ("gap_2", None, [block(0), block(3, 3), block(4, 4)]),
        # The gap limit (MAX_GAP_BLOCKS = 32): 32 lost blocks are filled with silence, 33 are refused.
        ("gap_32_accepted", None, [block(0), block(1, 33)]),
        # A forged seq is refused WITHOUT moving `next`: the real blocks 1 and 2 that follow are still taken.
        ("gap_33_rejected", None, [block(0), block(1, 34), block(1), block(2)]),
        ("gap_50_rejected", None, [block(0), block(1, 51), block(2, 52)]),
        ("gap_forged_first", None, [block(3, 40), block(0), block(1)]),
        ("duplicate_seq", None, [block(0), block(1), block(1), block(2)]),
        ("older_seq_after_newer", None, [block(0), block(1), block(3, 3), block(2, 2), block(4, 4)]),
        ("first_block_lost", None, [block(1, 1), block(2, 2)]),
        ("last_block_lost", None, [block(0), block(1), block(2)]),
        ("gap_then_short_last", None, [block(0), block(4, 3)]),
        # The turn tag. A tagged turn takes only its own tag; a dropped block does not move `next`.
        ("tag_match_gap", 5, [block(0, tag=5), block(2, 2, 5), block(3, 3, 5)]),
        ("tag_late_block_dropped", 5, [block(0, tag=5), block(1, tag=4), block(1, tag=5), block(2, tag=5)]),
        ("tag_zero_named", 0, [block(0, tag=0), block(1, tag=1), block(2, tag=0)]),
        ("tag_off_ignores_tags", None, [block(0, tag=9), block(1, tag=200), block(2, tag=0)]),
    ]


def bad_cases():
    good = encode_block(sine(505), 0, 0, 0)[0]
    short9 = encode_block([5], 0, 0, 0)[0]

    def with_n(payload: bytes, n: int) -> bytes:
        return payload[:2] + n.to_bytes(2, "little") + payload[4:]

    def with_index(payload: bytes, value: int) -> bytes:
        return payload[:7] + bytes([value]) + payload[8:]

    n506 = with_n(good + bytes(4), 506)            # the length IS right for n = 506; n is what is wrong
    return [
        ("bad_empty", b""),
        ("bad_short_4", good[:4]),
        ("bad_short_5", good[:5]),
        ("bad_short_8", good[:8]),
        ("bad_n0", with_n(short9, 0)),
        ("bad_n_506", n506),
        ("bad_n_65535", with_n(short9, 65535)),
        ("bad_index_89", with_index(good, 89)),
        ("bad_index_255", with_index(good, 255)),
        ("bad_length_long", good + b"\x00"),
        ("bad_length_long_4", good + bytes(4)),
        ("bad_length_short", good[:-1]),
        ("bad_length_short_4", good[:-4]),
        ("bad_length_n_mismatch", with_n(short9, 505)),
        ("bad_length_n_small", with_n(good, 9)),
    ]


def hex_pcm(samples) -> str:
    return b"".join((s & 0xFFFF).to_bytes(2, "little") for s in samples).hex()


def render() -> str:
    out = [
        "# Shared ADPCM block vectors for the harness cable link (frame type 0x07).",
        "# Generated by devices/harness-device/firmware/scripts/gen_adpcm_vectors.py - a third implementation, written",
        "# from the block description in docs/specs/cable-wifi-link.md and from neither half's code. Do not hand-edit.",
        "#",
        "#   enc  <name>  <pcm-le16-hex>  <index-in>  <seq>  <flags>  <payload-hex>  <index-out>  <tag>",
        "#   dec  <name>  <payload-hex>  <pcm-le16-hex>",
        "#   gap  <name>  <payload-hex,payload-hex,...>  <pcm-le16-hex>  <turn-tag or ->",
        "#   bad  <name>  <payload-hex>",
        "",
    ]
    for name, pcm, index_in, seq, flags, tag in enc_cases():
        payload, index_out = encode_block(pcm, index_in, seq, flags, tag)
        out.append(f"enc\t{name}\t{hex_pcm(pcm)}\t{index_in}\t{seq}\t{flags}\t{payload.hex()}\t{index_out}\t{tag}")
    out.append("")
    for name, payload in dec_cases():
        out.append(f"dec\t{name}\t{payload.hex()}\t{hex_pcm(decode_payload(payload))}")
    out.append("")
    for name, turn_tag, payloads in gap_cases():
        out.append(f"gap\t{name}\t{','.join(p.hex() for p in payloads)}\t{hex_pcm(assemble(payloads, turn_tag))}\t"
                   f"{'-' if turn_tag is None else turn_tag}")
    out.append("")
    for name, payload in bad_cases():
        try:
            decode_payload(payload)
        except ValueError:
            out.append(f"bad\t{name}\t{payload.hex()}")
        else:
            raise AssertionError(f"bad case {name} is accepted by the reference decoder")
    out.append("")
    return "\n".join(out)


def destinations():
    firmware = pathlib.Path(__file__).resolve().parent.parent
    daemon = firmware.parents[2] / "cli" / "src" / "cable" / "vectors"
    return [firmware / "test" / "vectors" / "adpcm_ima.txt", daemon / "adpcm_ima.txt"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if a copy on disk differs from what this run would write")
    args = ap.parse_args()
    text = render()
    paths = destinations()
    if args.check:
        stale = False
        for path in paths:
            # Compare as text with the newline style folded: a CRLF checkout (autocrlf) is the same file.
            if not path.exists() or path.read_bytes().decode("utf-8").replace("\r\n", "\n") != text:
                print(f"stale or missing: {path} - re-run {pathlib.Path(__file__).name}", file=sys.stderr)
                stale = True
        if not stale:
            print("up to date: " + ", ".join(str(p) for p in paths))
        return 1 if stale else 0
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))      # LF on every host: the bytes do not depend on the OS
        print(f"wrote {path} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
