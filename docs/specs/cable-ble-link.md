# Cable link over BLE (phone bridge): wire specification

Status: DRAFT. Source of truth for every byte and number that the dial (C firmware), the phone app (Kotlin) and the
desktop daemon must agree on for the BLE link. The design reasoning is in the phone bridge spec and plan
(`briefs/research/2026-10-30-phone-bridge-spec.md` and `2026-11-01-phone-bridge-plan.md` in the workbench repository).
The cable layers above this link are in [the Wi-Fi link spec](cable-wifi-link.md): frames (section 0), the TLS-PSK
profile (section 1), ADPCM voice (section 2). This file does not repeat them.

Vectors: `devices/harness-device/firmware/test/vectors/ble_status.txt`, with a byte-identical copy at
`devices/harness-device/phone-bridge/src/test/resources/ble_status.txt`. One script writes both:
`devices/harness-device/firmware/scripts/gen_ble_vectors.py` (`--check` fails when a copy differs).
`devices/harness-device/firmware/test/test_ble_vectors.py` checks that every UUID, constant and state name below is the one in
the vectors. A number changes in the script, never by hand in one place.

Labels. "[U]" means "not checked yet"; the named task settles it. Numbers marked "bench" were measured on the 2026-10-28
Pixel runs (image P6n, foreground app, screen on); they are not guarantees for background behavior.

## 0. Layers and roles

```
L3  messages      JSON: hello, welcome, voice.begin, ...        unchanged
L2  cable frame   A5 48 | ver | type | len:u16 | payload | crc16   unchanged
L1  secure link   TLS 1.2 PSK, dial <-> daemon, end to end         the phone cannot read or forge it
L0  byte stream   BLE LE credit-based L2CAP channel (CoC) on PSM 0x0080, then a WebSocket to the daemon
```

- The dial is the BLE peripheral and the TLS client. The phone is the BLE central. The daemon is the TLS server.
- The phone is a pipe: it copies bytes between the CoC stream and WebSocket binary messages and never parses them
  (section 6, rule 1). It never holds the PSK.
- The phone learns only sizes, timing, and the dial MAC (the PSK identity travels in the clear in the TLS handshake).

## 1. Identifiers

All UUIDs are random 128-bit values (not registered). One random base; the low byte of the first group is the attribute
index. UUID bytes go on the air last byte first (little endian); the vectors show both forms.

| Role | UUID |
|---|---|
| Service (in the scan response) | `e70301f0-bbe6-4d70-801c-657c8e99501d` |
| PSM characteristic | `e70301f1-bbe6-4d70-801c-657c8e99501d` |
| Relay-status characteristic | `e70301f2-bbe6-4d70-801c-657c8e99501d` |
| Home-hint characteristic | `e70301f3-bbe6-4d70-801c-657c8e99501d` |

The PSM is fixed at `0x0080` (dynamic range 0x0080..0x00FF). The PSM characteristic holds it as a u16 little endian
(`80 00`), read-only, no encryption needed. A central may use the characteristic or the fixed value; the fixed value is the
contract.

## 2. Advertising

- Connectable, undirected, legacy advertising, general discoverable, interval `adv_interval_ms` = 50 ms.
- Advertising packet: flags `02 01 06`, then the complete local name (AD type 0x09). Scan response: the 128-bit service UUID
  as a complete list (AD type 0x07). Both are byte-exact in the vectors (`adv` lines).
- Name: `harness-dial-<mac2>`. `<mac2>` is the last two bytes of the dial's BASE MAC (the PSK identity, for example
  `B8:1F:3F:CA:E5:58` gives `harness-dial-e558`), as four lower-case hex digits with no separator. The BLE radio address is
  the base MAC + 2, so the name matches the cable identity and not the radio address. The name is 17 characters; the packet
  is 22 bytes of the 31.
- A phone filters on the name prefix `harness-dial-` (it is in the primary packet) and the service UUID (scan response, active
  scan).
- Advertising stops after `adv_timeout_ms` = 30000 ms without a central. The dial shows "Phone not found". It advertises
  again on any screen wake (a touch, the PWR button) and, with no link, every `adv_retry_ms` = 120000 ms after it stopped: 30 s
  on the air, 120 s off, a fifth of the time. The phone-side trigger to reconnect is presence (section 6).
- One connection, one channel. Bonds: `max_bonds` = 2.

## 3. Link settings

- The dial never requests the 2M PHY. It never requests 2M PHY because the bench showed dropped links and 0-frame runs with
  BLE 5 features off (finding FB1). The phone may ask for 2M; the bench ran clean either way.
- The dial requests Data Length Extension (`dle_octets` = 251 bytes) `dle_request_delay_ms` = 1000 ms after connect.
- The dial makes no connection-parameter request in v1. The Pixel chose interval 15 ms, latency 1, supervision 5 s (bench).
- CoC settings of the dial (bench, must stay): SDU MTU `coc_mtu_bytes` = 4032, MPS `coc_mps_bytes` = 504, credits
  `coc_credits` = 8. The phone's MTU and credits decide what the dial may send; the dial clamps to the peer SDU MTU.
- Framing: NimBLE delivers one SDU per receive event; Android's socket may present a byte stream. Neither side relies on SDU
  boundaries. TLS records and cable frames carry their own lengths.

## 4. Security and admission

- Pairing: LE Secure Connections, numeric comparison. The dial is DisplayYesNo and shows a 6-digit number with Yes and No.
  Pairing is allowed only inside the window the user opens in Settings (`pair_window_ms` = 120000 ms).
- The channel is accepted only on a link that is encrypted, bonded and AUTHENTICATED (numeric comparison, not Just Works: a phone
  that says "no input, no output" gets an unauthenticated bond and is refused) with a link key of at least `key_size_min` = 16
  bytes. Otherwise the dial refuses the accept with "insufficient encryption". The dial calls `ble_gap_security_initiate` after connect, for a bonded and an unbonded peer. A
  link not encrypted `enc_deadline_ms` = 3000 ms after connect is dropped.
- The phone uses the secure channel call (`createL2capChannel(0x0080)`). Whether Android starts pairing by itself after a refusal
  is [U] until bench B0a; the dial does not rely on it.
- Bonds. A new bond is made only inside the pairing window (closed unless the user opened it), and never by evicting an old one:
  with `max_bonds` full a third pairing is refused, and "Forget phone" frees a slot. Outside the window the dial pairs without
  bonding, so nothing is written and the channel is refused.
- Abuse bounds. A link that opens and loses `churn_limit` = 3 channels without one reaching TLS is ended, and the dial waits
  `cooldown_ms` = 10000 ms before it advertises again. A peer that is not authenticated-bonded (any peer on a development build)
  may stay connected without a channel for `idle_cap_ms` = 10000 ms, then the link is dropped. After three TLS alerts in a row the
  wait is the flat `cooldown_ms` for such a peer; only an authenticated-bonded peer gets the escalating refused ladder (30 s to
  5 min), so an attacker cannot walk the ladder up against the real phone.
- Development flag: a build with `BLE_DEV_ALLOW_UNBONDED=1` accepts the channel without a bond, because TLS-PSK still
  authenticates both ends. It is off in every product config (a source test fails the build if it is on in the shipped
  overlay).
- Characteristics: relay-status and home-hint need an encrypted link (`READ_ENC`, `WRITE_ENC`). The PSM characteristic does
  not. A write to either also needs the channel rule above (bonded, authenticated, long key; the development flag waives it), so a phone
  that paired outside the window cannot steer the screen or the mode switch; the dial answers ATT error 0x05 (insufficient
  authentication). A relay-status write of a length other than 1 is rejected with 0x0D, like the hint.
- TLS-PSK runs end to end inside the CoC stream (Wi-Fi link spec, section 1). Dial deadline: `dial_tls_deadline_ms` = 5000 ms
  for the handshake after the channel opens, then it closes the channel. Daemon deadline: `daemon_tls_deadline_ms` = 3000 ms
  from the FIRST BYTE of the stream reaching the daemon, so the phone opens the WebSocket on the first CoC byte, not at
  connect.
- Not carried over BLE: firmware images (frame type 0x03), `net.set`, pet uploads. The dial refuses them unless the transport
  is USB. The daemon offers no firmware and no pets for the `ble` and `tunnel` transports.
- Log frames over BLE: warnings and errors only.

## 5. Characteristics

Both are one byte. Values and outcomes are in the vectors (`status` and `hint` lines).

### 5.1 Relay status (`e70301f2-...`): read, notify; the phone writes the value, the dial reads it

| Value | State name | Meaning |
|---|---|---|
| 0x00 | `idle` | no tunnel session |
| 0x01 | `tunnel_connecting` | WebSocket is opening |
| 0x02 | `tunnel_up` | WebSocket is up |
| 0x03 | `token_rejected` | the edge refused the Access token |
| 0x04 | `phone_no_network` | the phone has no internet |

Any other value reads as `unknown`: the dial shows it and does not treat it as an error. The phone writes this
characteristic on every state change. The dial shows "Phone: no internet" and similar text instead of a silent timeout. It is
a hint for the screen and is never trusted for data.

### 5.2 Home hint (`e70301f3-...`): write, encrypted

Value 0x01 = the phone sees the home network; 0x00 = it does not. The dial feeds it to the mode switch (home hint true for 10 s
returns the dial to Wi-Fi, subject to the flap cap and dwell). A write with a length other than 1 is rejected with ATT error
0x0D (invalid attribute value length). A write of a value above 1 is rejected with ATT error 0x80 (application error, value
out of range).

## 6. Relay rules (phone app)

1. Never parse payload bytes. A test pushes random bytes, including `A5 48`, and checks that they pass unchanged.
2. Open the WebSocket on the first CoC byte. Close it with code 1000 when the CoC closes.
3. Queue full: stop reading the source, so the dial's ring takes the stall. Queues are `relay_queue_bytes` = 65536 bytes each
   way. A queue full for `relay_full_close_ms` = 4000 ms closes both sides (TLS cannot resync).
4. WebSocket error: close the CoC channel only (keep the ACL link and the bond). Reopen the WebSocket on the back-off ladder
   1000, 2000, 4000, 8000, 16000, 30000 ms, then stay at 30000 ms. Each delay is the step plus or minus
   `ws_backoff_jitter_percent` = 20 percent, uniformly distributed (a 1000 ms step waits 800 to 1200 ms; the 30000 ms step
   waits 24000 to 36000 ms). The dial sees a channel close, resets TLS, and reconnects when the phone reopens the channel.
5. ACL link lost: the dial advertises again; the phone waits for presence.
6. Permissions denied or Bluetooth off: show the state; do not loop.
7. No secret in any log (Access token, URL query).
8. Write the relay-status characteristic on every state change.
9. WebSocket ping every `ws_ping_ms` = 25000 ms, to keep the tunnel idle timeout away.

## 7. Dial-side timing summary

| Constant | Value | What |
|---|---|---|
| `adv_interval_ms` | 50 | advertising interval |
| `adv_timeout_ms` | 30000 | advertising without a central |
| `adv_retry_ms` | 120000 | after "not found", advertise again |
| `enc_deadline_ms` | 3000 | connect to encrypted |
| `dial_tls_deadline_ms` | 5000 | TLS handshake on the dial |
| `daemon_tls_deadline_ms` | 3000 | TLS handshake at the daemon, from the first byte |
| `pair_window_ms` | 120000 | pairing window |
| `dle_request_delay_ms` | 1000 | DLE request after connect |
| `dle_octets` | 251 | requested data length |
| `coc_mtu_bytes` | 4032 | dial SDU MTU |
| `coc_mps_bytes` | 504 | dial MPS |
| `coc_credits` | 8 | credits granted |
| `write_cap_ms` | 2000 | a stalled SDU of 2 s closes the channel |
| `voice_stall_abort_ms` | 3000 | a stall of 3 s aborts a voice turn |
| `relay_queue_bytes` | 65536 | phone queue each way |
| `relay_full_close_ms` | 4000 | queue full, then close |
| `ws_ping_ms` | 25000 | WebSocket ping |
| `max_bonds` | 2 | bonded phones |
| `key_size_min` | 16 | link key bytes the dial requires |
| `churn_limit` | 3 | channels without TLS per link |
| `cooldown_ms` | 10000 | flat silence after abuse or an unproven refusal |
| `idle_cap_ms` | 10000 | untrusted peer, connected, no channel |
| `ws_backoff_jitter_percent` | 20 | back-off jitter, plus or minus |

## 8. Voice over BLE

ADPCM (frame type 0x07, Wi-Fi link spec section 2), negotiated by the `voice.begin` codec field. USB stays PCM. A record of one
block is 325 bytes on the wire (derived: the spec's CBC record), so one record fits one L2CAP PDU of 504 bytes.
