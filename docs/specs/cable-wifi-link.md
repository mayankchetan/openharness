# Cable link over Wi-Fi: wire specification

Status: DRAFT. Source of truth for the wire format. The design reasoning is in the Wi-Fi mode spec
(`briefs/research/2026-10-29-wifi-mode-spec.md` in the workbench repository, sections 4, 5 and 7).
This file holds only what an implementer on either side needs to be byte-exact.

What is implemented now: section 2 (the ADPCM voice block) and its golden vectors. Sections 1 and 3
describe work that is not written yet (tasks W7 to W11); they are here so that the three parts of the
wire format are in one place.

Labels. "[U]" means "from general knowledge, not yet checked against a document or code". Each [U]
names the task that settles it.

## 0. Layers

```
L3  messages      JSON: hello, welcome, voice.begin, net.set, ...      unchanged
L2  cable frame   A5 48 | ver | type | len:u16 | payload | crc16        unchanged, plus type 0x07
L1  secure link   TLS 1.2 with a pre-shared key; cable frames are plain bytes in the TLS stream   non-USB only
L0  byte stream   USB-Serial-JTAG | TCP | (later) WebSocket or BLE
```

L2 and L3 do not know which L0 carries them. The frame types are 0x01 JSON, 0x02 PCM, 0x03 firmware,
0x04 log, 0x05 pet, 0x06 screenshot and, new here, 0x07 ADPCM.

## 1. Secure link profile (TLS-PSK) - not implemented yet (W7, W8)

- TLS 1.2 only. Both ends pin the minimum and maximum version to TLS 1.2 [U: Node option names; confirm in W7].
- The dial is the TLS client. The daemon is the TLS server.
- PSK identity = the dial MAC string, upper case, for example `B8:1F:3F:CA:E5:58`. The identity is not secret.
- PSK = 32 random bytes from the daemon CSPRNG, one per dial.
- Suites: an ECDHE-PSK suite first, then a plain PSK suite with AES-128-GCM. The exact names that both mbedTLS and
  Node accept are fixed by the W0 interop test; the daemon then sets an explicit `ciphers` allow-list.
- Unauthenticated peers: handshake deadline 3 s from accept; a pool of 16 pending sockets, and the oldest pending
  socket is destroyed when the pool is full; one authenticated socket per identity, and a new one replaces the old.
- The firmware refuses frame type 0x03 and `net.set` unless the transport is USB. Firmware images are never
  offered over Wi-Fi.

## 2. ADPCM voice block (frame type 0x07, `CABLE_TYPE_ADPCM`)

### 2.1 Layout

The payload of a type-0x07 frame is a 5-byte prefix followed by exactly one WAV IMA ADPCM mono block.
All multi-byte fields are little-endian.

```
payload offset  size  field
0               2     seq      u16   block counter in the turn; 0 at voice.begin
2               2     n        u16   true sample count of this block, 1..505 (nominal 505)
4               1     flags    u8    bit0 = last block of the turn; the other bits are 0 on send, ignored on receive
--- WAV IMA mono block (payload[5:]) ---
5               2     pred     s16   sample 0, verbatim (the decoder's starting predictor)
7               1     index    u8    step index at sample 0, 0..88
8               1     reserved u8    0 on send, ignored on receive
9               4*k   data     nibbles, low nibble first, 8 samples per 4 bytes; the codes for samples 1..n-1
```

- `k = ceil((n - 1) / 8)`. The WAV block is `4 + 4k` bytes. The payload is `5 + 4 + 4k` bytes.
- A full block has n = 505 and k = 63: a 256-byte WAV block and a 261-byte payload. Check: (256 - 4) * 2 + 1 = 505.
- A short block pads its nibble run with zero nibbles up to a multiple of 8. The decoder decodes all `8k + 1`
  samples and keeps the first `n`. A decoder never reads the pad nibbles into its output, and it ignores their value.
- A receiver rejects a payload when any of these holds, and it must do so without reading past the buffer:
  fewer than 5 bytes; `n` is 0 or above 505; the length is not `5 + 4 + 4 * ceil((n - 1) / 8)`; `index` is above 88.
  The `reserved` byte and the high `flags` bits are not checked.

### 2.2 Decode rule

Standard IMA ADPCM tables: the 89-entry step table (7 .. 32767) and the index table `{-1, -1, -1, -1, 2, 4, 6, 8}`.
For each 4-bit code `c` with the current `step = STEP[index]`:

```
diff = step >> 3
if c & 1: diff += step >> 2
if c & 2: diff += step >> 1
if c & 4: diff += step
if c & 8: diff = -diff
pred = clamp(pred + diff, -32768, 32767)
index = clamp(index + INDEX[c & 7], 0, 88)
```

This is the reference form (a series of shifts). It is NOT bit-identical to ffmpeg's `adpcm_ima_wav` decoder, which
computes `((2 * (c & 7) + 1) * step) >> 3` in one step. The two agree on the step index at every sample (it depends
only on the codes) and differ in the predictor by a few least-significant bits, which then add up along a block.
Measured on the vectors of section 2.6 with ffmpeg 4.2.2: at most 15 LSB (of 32768) at any sample. That is far below
the quantisation step and has no effect on speech. It does mean ffmpeg cannot be used to check this decoder sample
for sample; section 2.6 says what ffmpeg is used for.

### 2.3 Encoder

- `pred` is the first sample of the block, written verbatim, so the first sample of every block is lossless.
- Each following sample is quantised by the classic IMA rule against the predictor the decoder will hold: take the sign
  (code bit 3) and the magnitude of `sample - pred`; set bit 2 if the magnitude is at least `step` and subtract it; halve
  `step`; set bit 1 if the remainder is at least the halved step and subtract; halve again; set bit 0 if the remainder
  is at least that. Then apply the decode rule above to update `pred` and `index`.
- `index` in the header is the step index at sample 0. The encoder carries `index` from the end of one block to the start
  of the next (smooth adaptation); the value after the last real code of a block (not after the pad nibbles) is what the
  next block starts from.

### 2.4 Lost, repeated and late blocks

- Every block restarts from its own `pred` and `index`, so a lost or damaged frame does not corrupt the rest.
- The receiver keeps `next = 0` at `voice.begin`. For each valid block: `seq < next` is a duplicate or a late block and
  is dropped; `seq > next` first inserts `(seq - next) * 505` zero samples; then the block is decoded and `next = seq + 1`.
- The receiver counts decoded PCM bytes (not payload bytes) against its length cap.

### 2.5 Negotiation

| Step | Message |
|---|---|
| Dial advertises | `hello.voiceCodec = "adpcm-v1"` |
| Daemon offers | `welcome.features` includes `"voice.adpcm"`, only to a dial that advertised `adpcm-v1` |
| Dial chooses | `voice.begin.codec = "adpcm"` when the feature is present and the dial wants it; otherwise PCM as before |
| USB | PCM by default. ADPCM over USB only behind the firmware build flag `DEVICE_VOICE_ADPCM_USB` (task W4) |

The daemon decodes ADPCM on whatever transport the dial uses; it has no "Wi-Fi only" rule. The dial decides.
`CABLE_PROTO_VERSION` is 4 on the daemon (3 before this change; the firmware's value moves to 4 with the firmware
sender, task W4). Firmware feature bit `CABLE_FEATURE_ADPCM = 1u << 8`; frame type `CABLE_TYPE_ADPCM = 0x07`.

Daemon behavior for one turn:

- `voice.begin.codec` equal to `"adpcm"` makes the turn an ADPCM turn. Any other value, or none, is a PCM turn, as before.
- An ADPCM turn takes type-0x07 frames only; a type-0x02 frame in it is ignored. A PCM turn takes type-0x02 frames only.
  A type-0x07 frame while no turn is open is ignored, like PCM.
- A malformed block is counted and dropped; the turn goes on.
- The turn's length cap counts decoded bytes, silence included, and is checked before the silence for a gap is built,
  so a damaged `seq` cannot make the daemon allocate a long run of zeros. Over the cap the turn is dropped.
- The dial log gets `cable: voice <s>s (<KB> KB) -> stt - adpcm <blocks> blocks, <lost> lost, <repeated> repeated, <bad> bad`
  for each turn, and the same four counts in the periodic `link` line once ADPCM has been used on the port.

### 2.6 Golden vectors and the ffmpeg check

`devices/harness-device/firmware/scripts/gen_adpcm_vectors.py` writes `adpcm_ima.txt` into
`devices/harness-device/firmware/test/vectors/` and into `cli/src/cable/vectors/`. It is written from this section only.
Line kinds (tab-separated, PCM as little-endian int16 hex):

```
enc  <name> <pcm> <index-in> <seq> <flags> <payload> <index-out>
dec  <name> <payload> <pcm>
gap  <name> <payload,payload,...> <pcm>
bad  <name> <payload>
```

- `enc`: the encoder must produce exactly `payload` and end on `index-out`.
- `dec`: the decoder must return exactly `pcm` (n samples).
- `gap`: the blocks as they arrived; the receiver of section 2.4 must produce exactly `pcm`.
- `bad`: the decoder must reject the payload.

ffmpeg check (needs ffmpeg; the test skips when it is absent). Wrap `payload[5:]`, zero-padded to 256 bytes, in a WAV file:
`RIFF`/`WAVE`; a `fmt ` chunk of 20 bytes with format tag 0x0011, 1 channel, 16000 Hz, average bytes per second
`16000 * 256 / 505` (8111), block align 256, 4 bits per sample, `cbSize` 2, and 505 samples per block; a `fact`
chunk with the total sample count; then the `data` chunk. These fields were checked against ffmpeg 4.2.2 and 4.2.3:
it decodes the right number of samples with the right step sequence. Because of the rounding difference in section
2.2, the test checks ffmpeg against a decoder that uses ffmpeg's rounding (exact equality), and against the vector
samples within a small bound.

## 3. Provisioning messages - not implemented yet (W9, W10)

The USB cable is the authorisation. These messages are accepted only on a USB transport and are never echoed or logged.

```
daemon -> dial   {"t":"net.set","ssid":"...","pass":"...","host":"192.168.x.y","port":18480,"key":"<base64 of 32 bytes>","mode":1}
dial -> daemon   {"t":"net.state","ok":true,"ssid":"...","saved":true}
```

- Validation on the dial: SSID 1 to 32 bytes; password 0 or 8 to 63 bytes; host a dotted IPv4; port 1024 to 65535;
  key decodes to exactly 32 bytes.
- NVS namespace `pair`: `wl_ssid`, `wl_pass`, `wl_host`, `wl_port`, `wl_key`, then `wl_ver = 1` written last as the
  commit marker. A dial with no `wl_ver` treats Wi-Fi as unprovisioned.
- The firmware logs lengths only, never values. The daemon logs no payload.
- The daemon stores the key only after `ok:true`. Default daemon port: 18480.
