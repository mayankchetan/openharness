# Cable link over Wi-Fi: wire specification

Status: DRAFT. Source of truth for the wire format. The design reasoning is in the Wi-Fi mode spec
(`briefs/research/2026-10-29-wifi-mode-spec.md` in the workbench repository, sections 4, 5 and 7).
This file holds only what an implementer on either side needs to be byte-exact.

What is implemented now: section 2 (the ADPCM voice block) and its golden vectors, section 4 (keepalive timing, W13), section 3 (the provisioning
messages over the USB cable, W9 and W10) and section 7 (the settings report and the CLI, W16). Section 1 describes work that is not finished (W8, W11); it is here so that
the parts of the wire format are in one place.

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
8               1     tag      u8    the turn tag: the sender's counter for this turn (= voice.begin.tag); 0 when it sends none
9               4*k   data     nibbles, low nibble first, 8 samples per 4 bytes; the codes for samples 1..n-1
```

- `k = ceil((n - 1) / 8)`. The WAV block is `4 + 4k` bytes. The payload is `5 + 4 + 4k` bytes.
- A full block has n = 505 and k = 63: a 256-byte WAV block and a 261-byte payload. Check: (256 - 4) * 2 + 1 = 505.
- A short block pads its nibble run with zero nibbles up to a multiple of 8. The decoder decodes all `8k + 1`
  samples and keeps the first `n`. A decoder never reads the pad nibbles into its output, and it ignores their value.
- A receiver rejects a payload when any of these holds, and it must do so without reading past the buffer:
  fewer than 5 bytes; `n` is 0 or above 505; the length is not `5 + 4 + 4 * ceil((n - 1) / 8)`; `index` is above 88.
  The `tag` byte and the high `flags` bits are not checked by the decoder; the receiver of section 2.4 holds a block to its
  turn's tag. The tag byte is the reserved byte of the WAV block. A tool that wraps `payload[5:]` in a WAV file clears it,
  because ffmpeg 4.2.x reads that byte as the high byte of the step index (section 2.6).

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
- The receiver keeps `next = 0` at `voice.begin`, and the turn's tag `T` when `voice.begin` carried one (section 2.5). For
  each block that decodes, in this order:
  1. `T` is set and the block's tag is not `T`: a late block of an earlier turn. Dropped, counted `bad`. `next` is unchanged.
  2. `seq < next`: a duplicate or a late block. Dropped, counted `repeated`.
  3. `seq - next > MAX_GAP_BLOCKS` (32 blocks, about 1 s): Dropped, counted `bad`. `next` is NOT moved.
  4. Otherwise `seq > next` first inserts `(seq - next) * 505` zero samples; then the block is decoded and `next = seq + 1`.
- Frame fields are not trusted: a port, a LAN socket or a phone relay can send any `seq`. Step 3 means a forged large `seq`
  makes at most 32 blocks of silence (32 KB) and cannot push `next` past the real blocks, which would drop them all as
  late. The price is that a real outage longer than 32 blocks loses the rest of the turn; an outage of 1 s already
  spoils the utterance, so this is accepted.
- The tag byte is one counter per `voice.begin`, so a block of turn N that lands in turn N+1 is told apart (type-0x07
  frames carry no turn id). A turn without a tag, such as one from an older dial, takes any tag.
- The receiver counts decoded PCM bytes (not payload bytes) against its length cap.

### 2.5 Negotiation

| Step | Message |
|---|---|
| Dial advertises | `hello.voiceCodec = "adpcm-v1"` |
| Daemon offers | `welcome.features` includes `"voice.adpcm"`, only to a dial that advertised `adpcm-v1` |
| Dial chooses | `voice.begin.codec = "adpcm"` and `voice.begin.tag` (u8, a counter that goes up by one at each `voice.begin` and wraps) when the feature is present and the dial wants it; otherwise PCM as before, with no `tag` |
| USB | PCM by default. ADPCM over USB only behind the firmware build flag `DEVICE_VOICE_ADPCM_USB` (task W4) |
| Wi-Fi | ADPCM whenever the welcome has `voice.adpcm` (task W12): the dial asks the open transport at `hello` and at every `voice.begin` (`voice_transport_is_usb`), so the codec follows the link. A Wi-Fi dial also keeps a 96 KB PCM ring (98,304 B, 3.07 s, PSRAM) in place of the 32 KB USB ring (1.02 s), so a 1.5 s stall of the link does not end the turn; a stall longer than the ring still does, with the same abort as on USB |

The daemon decodes ADPCM on whatever transport the dial uses; it has no "Wi-Fi only" rule. The dial decides.
`CABLE_PROTO_VERSION` is 4 on both sides (3 before this change; the daemon moved in task W3, the firmware with its
sender in task W4). A firmware on a USB link and built without `DEVICE_VOICE_ADPCM_USB` sends no `hello.voiceCodec` (it advertises the codec
only where it would use it), so a daemon never offers it `voice.adpcm`; on Wi-Fi it sends it (task W12). Firmware feature bit `CABLE_FEATURE_ADPCM = 1u << 8`; frame type `CABLE_TYPE_ADPCM = 0x07`.

Daemon behavior for one turn:

- `voice.begin.codec` equal to `"adpcm"` makes the turn an ADPCM turn only when this session negotiated it: the dial's
  `hello.voiceCodec` was `"adpcm-v1"`, so the welcome offered `voice.adpcm`. Without that, the turn is a PCM turn and the
  log says so once per port. Any other value, or none, is a PCM turn, as before.
- `voice.begin.tag`, an integer 0 to 255, is the turn's tag. Absent, or any other value: no tag check (older dials).
- An ADPCM turn takes type-0x07 frames only; a type-0x02 frame in it is ignored. A PCM turn takes type-0x02 frames only,
  and a type-0x07 frame in it is ignored. Each ignored frame is counted as `wrong-type`; the first one of a turn is logged.
  A frame of either type while no turn is open is ignored, uncounted.
- A malformed block, a block with another turn's tag and a block more than 32 blocks ahead are counted `bad` and dropped;
  the turn goes on.
- The turn's length cap counts decoded bytes, silence included, and is checked before the silence for a gap is built.
  A gap is at most 32 blocks (section 2.4), so the silence a damaged `seq` can ask for is bounded. Over the cap the turn is
  dropped.
- The dial log gets `cable: voice <s>s (<KB> KB) -> stt - adpcm <blocks> blocks, <lost> lost, <repeated> repeated, <bad> bad`
  for each turn, with ` - <n> wrong-type frames` after it when there were any, and the same counts in the periodic `link`
  line (`..., <bad> bad, <n> wrong-type`) once ADPCM has been used on the port.

### 2.6 Golden vectors and the ffmpeg check

`devices/harness-device/firmware/scripts/gen_adpcm_vectors.py` writes `adpcm_ima.txt` into
`devices/harness-device/firmware/test/vectors/` and into `cli/src/cable/vectors/`. It is written from this section only.
Line kinds (tab-separated, PCM as little-endian int16 hex):

```
enc  <name> <pcm> <index-in> <seq> <flags> <payload> <index-out> <tag>
dec  <name> <payload> <pcm>
gap  <name> <payload,payload,...> <pcm> <turn-tag or ->
bad  <name> <payload>
```

- `enc`: the encoder must produce exactly `payload` (the tag in byte 8) and end on `index-out`.
- `dec`: the decoder must return exactly `pcm` (n samples).
- `gap`: the blocks as they arrived and the turn's tag (`-` = none); the receiver of section 2.4 must produce exactly
  `pcm`. The cases cover a gap of exactly 32 (filled), 33 and 50 (refused, `next` unmoved, real blocks after them kept),
  a forged first block, and a tagged turn that gets a block of the turn before.
- `bad`: the decoder must reject the payload.

ffmpeg check (needs ffmpeg; the test skips when it is absent). Wrap `payload[5:]`, zero-padded to 256 bytes and with byte 3
of the block (the tag) cleared, in a WAV file. The clearing is needed: ffmpeg 4.2.3 reads the block header's step index as 16
bits, so a tag other than 0 is the high byte of the index to it and it refuses the block (`step_index[0] = 2307` for index 3
and tag 9). The daemon decodes with its own decoder and never meets this; only WAV tools do.
`RIFF`/`WAVE`; a `fmt ` chunk of 20 bytes with format tag 0x0011, 1 channel, 16000 Hz, average bytes per second
`16000 * 256 / 505` (8111), block align 256, 4 bits per sample, `cbSize` 2, and 505 samples per block; a `fact`
chunk with the total sample count; then the `data` chunk. These fields were checked against ffmpeg 4.2.2 and 4.2.3:
it decodes the right number of samples with the right step sequence. Because of the rounding difference in section
2.2, the test checks ffmpeg against a decoder that uses ffmpeg's rounding (exact equality), and against the vector
samples within a small bound.

## 3. Provisioning messages (W9 daemon, W10 firmware)

The USB cable is the authorisation. The dial answers a `net.*` message only on the USB transport, and the daemon sends one only
on a port that says `usb`. Over any other link the dial replies `err:"transport"` with no state, and changes nothing. The password
and the key never leave the cable, are never logged on either side, and are never sent back.

Negotiation. The dial's `hello` carries `"net":"v1"` (on the cable only). A daemon that sees it lists `"net"` in `welcome.features`
(on the cable only). The dial answers no `net.*` unless its last `welcome` listed `"net"` (`err:"feature"`), and the daemon sends
none to a dial that did not say `net` (it answers the caller `unsupported` itself). The bit is `CABLE_FEATURE_NET` (1 << 9).

Messages, daemon to dial. Every one may carry `id` (a whole number from 1 to 65535), which the answer repeats.

```
{"t":"net.set","id":n,"ssid":"...","pass":"...","host":"192.168.x.y","port":18480,"key":"<base64 of 32 bytes>","mode":1}
{"t":"net.get","id":n}
{"t":"net.clear","id":n}
{"t":"net.mode","id":n,"mode":0}
{"t":"net.key","id":n,"key":"<base64 of 32 bytes>"}         rotate: a new key for a dial that is already set
{"t":"net.host","id":n,"host":"192.168.x.y","port":18480}   the daemon's address changed; same network, same key
```

`net.mode`, `net.key` and `net.host` need a whole configuration (`err:"not_provisioned"` otherwise). `net.set` replaces any
configuration; a refusal or a power cut during it leaves the dial with none, never with half of two.

The one answer, dial to daemon:

```
{"t":"net.state","id":n,"ok":true,"saved":true,"ssid":"...","host":"192.168.x.y","port":18480,"mode":1}
{"t":"net.state","id":n,"ok":false,"err":"pass","saved":false}
```

- `ok` is whether the dial did it. `saved` is whether it holds a whole configuration (its commit marker is set); when it does,
  `ssid`, `host`, `port` and `mode` say what it holds. None of them is a secret. The password and the key are never in an answer.
- `err` names the first bad FIELD, in the order `ssid`, `pass`, `host`, `port`, `key`, `mode`, or a reason: `not_provisioned`,
  `storage` (NVS would not take a write, or the writing task could not start), `busy` (another write is still running; nothing changed), `op` (a `net.` message this firmware does not have), `transport`, `feature`. Never a value.
  A `transport` or `feature` refusal carries no state at all.

Validation, the same on the dial (`config_net_set`) and in the daemon (`cli/src/lib/dialNetRules.ts`): SSID 1 to 32 bytes and a
password of 0 or 8 to 63 bytes, neither with a control character; host a dotted IPv4 (four parts of 1 to 3 digits, none above 255,
no leading zero); port 1024 to 65535 and a whole number; key the standard base64 of exactly 32 bytes (44 characters with one `=`,
no whitespace, canonical trailing bits); mode 0 or 1. The daemon also refuses a host that is not a private (RFC 1918) address.

NVS namespace `pair`: `wl_ssid`, `wl_pass`, `wl_host`, `wl_port` (u16), `wl_key` (blob, 32 bytes), `wl_mode` (u8: 1 = join Wi-Fi
when the USB cable is unplugged, 0 = USB only), and `wl_ver = 1`, the commit marker.
- A set removes `wl_ver` first, writes the other keys, and writes `wl_ver` last. A clear removes `wl_ver` first. A dial with no
  `wl_ver`, a marker of another version, or any key missing or invalid treats Wi-Fi as unprovisioned.
- `config_clear_all` (factory reset) takes every key of the namespace, these included.
- NVS is log-structured: erasing a key, a clear or a factory reset removes it logically, and the old bytes (the password, the key) stay in flash
  until garbage collection overwrites them. The real fix is NVS encryption (spec 5.4, hardening); until then the dial's flash is plaintext to
  whoever holds it.
- Every change to a held configuration (`net.key`, `net.host`, `net.mode`, as well as `net.set`) removes `wl_ver` first, writes the change,
  and writes `wl_ver` last, so that a power cut leaves the dial with the old configuration or with none, never half of a change.
- A host the daemon could be at is a dotted IPv4 that is not 0.0.0.0, 255.255.255.255, loopback (127/8) or multicast (224/4); the daemon
  also refuses anything that is not a private address. An all-zero key is refused on both sides.
- A reply with no state omits `saved`. While a write is running, `net.get` is answered `ok:false, err:"busy"`.

Who writes. The cable reader task has its stack in PSRAM and does not write flash. It parses and checks a write (`set`, `clear`, `mode`, `key`,
`host`), copies it into one static slot, and starts the `net_store` task (internal stack, 4,096 B), which writes NVS, sends the `net.state`
reply, overwrites the slot and ends. One write at a time: a second one is answered `busy`. `get` only reads and is answered at once.

Logging. The firmware logs lengths only (`ssid_len`, `pass_len`, `host_len`), because every ESP_LOG line is framed into the
daemon's dial log. The daemon logs the op, the id and the outcome of a request, nothing of its fields. The daemon's local route
(`POST /api/dial/net/*`) logs only its path.

Daemon side.
- The daemon stores the key in `dial-net.json` only after `net.state` `ok:true`. A refusal or a 5 s timeout leaves the keystore
  untouched. If the keystore will not write after the dial agreed, the dial is cleared again (or given its old key back on a rotate).
- When this computer's LAN address changes while a dial is plugged in, the next attach reads `net.get` and, if the dial holds another
  address, sends `net.host`. A dial that is already wireless has to be fixed over the cable.
- Default daemon port: 18480 (`CABLE_NET_PORT`). No mDNS: the dial connects to the stored address.
- `harness dial wifi set|show|forget|rotate|on|off|firewall`. The password is typed at a prompt that shows nothing, or read as one line
  of stdin when stdin is not a terminal; it is never an argument and never read from the environment. `firewall` prints the Windows
  firewall command (inbound TCP on the Wi-Fi port, private profile, local subnet) and does not run it.

## 4. Keepalive timing (task W13)

Without the field below, the link keeps the numbers it has always had: the daemon pings every 5 s, the dial calls 15 s without any byte
a dead daemon, and the daemon calls 20 s without any byte a dead dial. The daemon's window is the later one on purpose: one late
message must not trip both sides at once and have each conclude the other left. The dial greets every 15 s while a session is up
(`HELLO_SESSION_MS`), and every 2 s while it is not; neither number moves with the field.

Negotiation. The dial's `hello` carries `"keepalive":"v1"` (on every transport). Only then, and only when the daemon is
configured for it, the `welcome` carries `"keepaliveMs":K`. A dial that did not say `keepalive` is never sent the field: an older
firmware would call a slower ping a dead daemon every 15 s. The welcome is sent again for every hello, so the field is repeated
and a daemon that restarts without the setting takes the link back to the fixed numbers.

The rule (one table, `firmware/test/vectors/keepalive.txt`, read by the dial's host test `test_cable_keepalive.c` and by the daemon's
`keepalive.spec.ts`):

| | value |
|---|---|
| K | `keepaliveMs` rounded to a whole millisecond and clamped to 5000..60000. Absent, not a number or NaN: 5000 |
| daemon pings every | K |
| the dial calls a dead daemon after | 3 x K without any byte (15 s .. 180 s) |
| the daemon calls a dead dial after | 3 x K + 5 s without any byte (20 s .. 185 s) |

So K can only slow the ping down, never speed it up; K = 5000 is today's link exactly. Both sides compute their window from the same K, and
the daemon's is always 5 s later than the dial's. The dial answers every ping with a `pong`, so the daemon hears from it at least every K.

Configuration. The daemon reads `CABLE_KEEPALIVE_MS` (`cli/src/config/env.ts`); 0 or unset (the default) is off and the welcome has no field.
Nothing in the repository sets it: the battery plan decides the value. The dial applies the field in `session_up` (every welcome) and goes
back to 5000 when the session ends.

## 5. The USB probe of a wireless dial (task W15)

A dial that runs on Wi-Fi watches the USB port. While a host sends start-of-frame packets it installs a small USB driver and says
`{"t":"hello","product":"harness","proto":N,"probe":1}` every 2 s. It looks for exactly one answer: a JSON frame whose top-level `t` is
`"welcome"`. Nothing else is read; every other frame is dropped.

The daemon answers a hello whose `probe` is the number 1 (not `true`, not `"1"`) with `{"t":"welcome","proto":N,"app":"...","probe":1}` and
nothing more. That hello is not a greeting: no capability is read, no dial is attached or reported, no agent list, machine list, pet or
firmware offer is sent, and the state the daemon keeps for a greeted dial is not touched. The dial that then reboots into USB mode sends a
normal hello and is greeted in full. A daemon older than this change ignores the field and answers with its full welcome and pushes; the
probe finds that welcome only if the whole frame fits the cap below, and drops the rest. The probe therefore needs a W15 or newer daemon: an old
daemon's full welcome (machine, selected machine, features) above 1,016 bytes of payload is dropped, and that dial would not come back to USB.

The probe's decoder holds one frame of at most 1,024 bytes. A frame over that is noise to it, so the daemon's probe welcome is kept
under 256 bytes whatever the machine is called (`cableSession.probe.spec.ts`). The rule that turns welcomes into a switch to USB (5 s of
fresh welcomes, none older than 3 s) is `firmware/main/mode_switch.c`.

## 6. The mode switch on the dial (task W15)

Pure rules: `firmware/main/mode_switch.c` (boot table, timers, busy gate, flap guard). Glue: `mode_glue_core.c` (tested on the host against a simulated
dial), `mode_glue.c` (IDF calls), `usb_probe.c` / `usb_probe_core.c` (section 5), `mode_record.c` (RTC record), `config_store.c` (three NVS values).

NVS, namespace `pair`: `wl_mode` (net.set; 0 or 1 only), `stay_wl` and `boot_usb` (one byte each, true only for the byte 1). The wireless target the
switch sees is `config_load_wl_effective()`: Wi-Fi only when the Wi-Fi settings are whole and `wl_mode` is exactly 1; anything else is no target and
the dial boots USB and never leaves it. `net.clear` and the factory reset take `stay_wl` too. Bluetooth (`wl_mode` 2) is not storable and not a target
until it has a transport.

Boot order in `app_main`: factory-reset countdown, `mode_glue_boot()` (record, NVS, SOF flag read once, at least 20 ms after the reset), the radio only
if the decision is Wi-Fi, the link, the focus restored, the `mode_sw` task started with the mode that really started. A switch is: log the reason
(`last_words`: "mode switch"), seal the focus and the flap count in the RTC record, store `boot_usb`, restart. Three switches in a row, then none until 10
minutes of uptime in one mode; none before 60 s of uptime.

## 7. The settings report, the link-state text, the Link screen and the CLI (task W16)

Settings report. A dial may add `linkMode` to the `settings` object of `hello` and `settings.state`: the string `"usb"` or `"wifi"`, the link the
dial chose for the time its cable is out (`wl_mode` 0 or 1). The daemon keeps it as `DeviceSettings.linkMode` for those two strings only. Any other
value, or none, leaves the key out and the rest of the report parses as before (`cableSession.spec.ts`, the test named "W16"). It is read-only: the
daemon drops it from a `settings.set` patch, and no firmware reports it yet. The settings never carry the Wi-Fi password, the key or the address of
the daemon (section 3).

`stay_wl` is on no wire path. No message of this protocol and no CLI verb sets or reads it (`dialWifiHelp.spec.ts` checks the help and the usage
text). It lives in the dial's NVS (section 6) and has one writer: the Keep Wi-Fi toggle on the dial's Link screen (below), which turns it off as well
as on. That is deliberate: a dial in Wi-Fi with `stay_wl` on never looks at the cable, so the only way back to USB is the toggle on the dial.

Link-state text. While the dial is not connected it shows the wordmark and, in Wi-Fi mode, one dim line under it from `net_state_caption`
(`net_state.c`, task W11; drawn by `render_brand` in `ui_habitat.c`): `Joining Wi-Fi` (JOINING), `Wi-Fi: no address` (NO_IP), `Daemon not reachable`
(DAEMON_UNREACHABLE), `Plug in USB` (AUTH_FAILED: the key was refused; the cable brings the dial back to USB, and with Keep Wi-Fi on the Link screen does). OFF and UP show nothing. The line never shows the network name, the password or the key.

CLI. `harness dial wifi set|show|forget|rotate|on|off|firewall` was built in W9. W16 added only the sentence that the dial joins 2.4 GHz networks
only (the help and the output of `set`). There is no `stay_wl` verb. The user note is `docs/dial-wifi.md`.

The Link screen (user decisions 2026-10-31 and 2026-11-01). One screen on the dial (`LINK` in `ui_habitat.c`), two rows, shown only when the Wi-Fi
settings are whole: `Link: Wi-Fi` or `Link: USB only` (tap flips `wl_mode`, `config_net_set_mode`) and `Keep Wi-Fi: on` or `off` (tap flips `stay_wl`).
Under them a hint and one line of link state (`net_state_line`: `Wi-Fi: off`, `joining`, `no address`, `daemon not reachable`, `key refused`,
`connected`). A dial with no Wi-Fi set up shows one sentence that says how to set it up. The screen opens with the EDGE SWIPE: a drag that starts
in the bottom band of the glass (y >= 400) and goes 55 px or more up, more up than sideways. It works from the brand screen, so a dial that cannot
reach its daemon can still be reached, and from the home face. The same stroke, a sideways swipe and the back arrow close it. A drag that starts in the
band is not reported as a terminal scroll. The worker task (not the render or touch task) reads and writes NVS and then calls
`mode_glue_settings_changed()`, so the running mode switch re-reads `wl_mode` and `stay_wl`.

Picking USB while Wi-Fi runs. `wl_mode` unset while a wireless mode runs is the user's pick of USB: `ms_step` (`mode_switch.c`, test H12) reboots into
USB through the same busy gate and glue sequence as an automatic switch (log reason, seal, `boot_usb` = USB, restart; a refused restart is undone). It
needs no cable, no probe and no daemon; the dwell and the flap cap do not hold it, and it restarts the flap count. USB with no target still never leaves.
Turning Keep Wi-Fi off on a dial in Wi-Fi with the cable in lets the probe run again, and the normal rule (5 s of welcomes) takes it to USB.
