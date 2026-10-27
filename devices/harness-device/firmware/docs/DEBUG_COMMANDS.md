# Debug commands: screenshot and touch injection

A host script can do two things over the cable that the daemon never does:

- **`debug.shot`** returns the current 466x466 picture of the face.
- **`debug.tap`, `debug.press`, `debug.swipe`** play a touch on the face.

The panel is write-only and the board has one USB port. Before this, the only way to see a screen was
a camera. Now a script can drive any screen and look at the result.

Source: `main/debug_cmd.[ch]` (pure logic, tested on the host), `main/debug_cmd_json.c` (parse and
reply), `main/cable_debug.[ch]` (the device half), and two small hooks: `capture_service()` in
`ui/habitat/display_habitat.c` and the `dbg_inject_poll` block in `ui/habitat/touch_habitat.c`.

## Switch

**Enabled by default** in this fork. Nothing runs until the host sends a command, but anyone with
the cable can send one. A build for a device you do not hold should remove it:

```
idf.py -DDEVICE_NO_DEBUG_COMMANDS=1 build
```

This removes the three source files, the renderer hook, the touch hook, the `debug` field in `hello`
and the dispatch in `cable_client.c`. The flash cost of the feature is 5,728 bytes (measured: 3,362,736
bytes with it, 3,357,008 without). It is a CMake switch, not Kconfig: the other build options in
`main/CMakeLists.txt` work this way, and a Kconfig entry would change `sdkconfig`.

## How the host talks to it

Use the normal cable framing (`cable_frame.h`: `A5 48 | ver | type | len:u16 | payload | crc16`).

| Frame type | Direction | Payload |
| --- | --- | --- |
| `0x01` JSON | both | one JSON object with a `"t"` key |
| `0x06` SHOT | device to host | one screenshot chunk (binary, below) |

- No `welcome` and no session is needed. The device answers `debug.*` at any time. A script that only
  wants a picture does not have to act as the daemon.
- The device sends `hello` every 2 s when it has no session. The `hello` of a build with this feature
  has `"debug":"v1"`. A script can wait for it to know the build supports these commands.
- The device also puts `hello` frames and, when a session is up, `log` frames (`0x04`) on the wire.
  Console text from the ROM and the bootloader can appear between frames too. A host decoder must skip
  what it did not ask for. Match replies by `"t"` and `"id"`.
- A `welcome` may list `"debug"` in `features` (`CABLE_FEATURE_DEBUG`). It is informational: the device
  answers with or without it.
- Unknown extra keys in a request are ignored. A message with the name of a `debug.*` command that this
  build does not have is counted as unknown, like any other.

`id` is an optional whole number from 0 to 65535 in every request. The device repeats it in every
reply to that request. It is 0 when the request has none. Send a different `id` for each request, so a
late reply to an old request cannot be taken for the new one.

## Screenshot

### Request

```json
{"t":"debug.shot","id":7}
```

### Reply, in this order

1. One JSON frame:

```json
{"t":"debug.shot.begin","id":7,"w":466,"h":466,"fmt":"rgb565be","bytes":434312,"chunk":1024,"chunks":425}
```

2. `chunks` binary frames of type `0x06`. Payload, all little-endian:

```
offset  size  field
0       2     id      u16, the request id
2       2     index   u16, 0 .. chunks-1, in order
4       n     pixels  n = chunk (1024) for every frame except the last; the last has bytes - (chunks-1)*chunk = 136
```

3. One JSON frame:

```json
{"t":"debug.shot.end","id":7,"chunks":425,"crc32":3421780262}
```

`crc32` is CRC-32/ISO-HDLC (the same as `zlib.crc32` in Python) over the pixel bytes of all chunks in
order, as the device sent them. The numbers above are an example: 3421780262 is the CRC of the ASCII
text `123456789`, not of a picture.

### Pixels

- `fmt` `rgb565be`: two bytes per pixel, **high byte first**. Bits are `RRRRRGGG GGGBBBBB`.
- Row by row, top row first, left pixel first. No padding between rows. Size is `w*h*2` = 434,312
  bytes.
- The bytes are exactly what the renderer produces for this panel. The CO5300 needs this byte order,
  so the device does no conversion.

Python, to turn the bytes into an image:

```python
import numpy as np
v = np.frombuffer(pixels, dtype=">u2").reshape(h, w).astype(np.uint32)
r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
rgb = np.dstack(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)).astype(np.uint8)
```

### How the host detects loss

A picture is good only when all of these hold:

1. `begin` arrived, with the `id` that was sent.
2. The chunk `index` values are exactly 0, 1, 2, ... `chunks-1` with no gap and no repeat, and each has
   the right `id`.
3. The total pixel bytes equal `bytes`.
4. `end` arrived with the same `chunks`, and `zlib.crc32` of the pixel bytes equals its `crc32`.

Each frame also has its own CRC-16, so a damaged frame never reaches the host as data. It is dropped,
and rule 2 shows the gap.

If the device cannot finish (the host stopped reading), it **does not send `end`**. A half picture is
never shown as complete. Wait for `end` with a time limit. Retry on a timeout.

### Errors

```json
{"t":"debug.error","id":7,"cmd":"shot","reason":"memory"}
```

| `reason` | Meaning |
| --- | --- |
| `memory` | PSRAM cannot take the 434,312 bytes and still keep 512 KiB free. Nothing was allocated. |
| `asleep` | The panel is off, or has not drawn its first frame. Wake it with a tap, then retry. |
| `busy` | Another capture is in progress. |
| `timeout` | The renderer did not answer in 2 s. |
| `shape` / `range` | `id` is not a whole number (shape) or not in 0..65535 (range). |

## Touch

### Requests

```json
{"t":"debug.tap","id":1,"x":233,"y":233}
{"t":"debug.press","id":2,"x":233,"y":233,"ms":700}
{"t":"debug.swipe","id":3,"x1":380,"y1":233,"x2":80,"y2":233,"ms":250}
```

| Command | Keys | What the face feels |
| --- | --- | --- |
| `debug.tap` | `x`, `y` | A press for 60 ms at the point, then a release. |
| `debug.press` | `x`, `y`, `ms` (50..5000) | A press held for `ms`, then a release. Use `ms` of 650 or more for a long press (the pet's hold starts at 650 ms). |
| `debug.swipe` | `x1`, `y1`, `x2`, `y2`, `ms` (20..3000) | A press at the start, a straight line to the end over `ms`, 8 ms at the end point, then a release there. |

`x` and `y` count pixels from the top left, 0..465.

### Validation

- Every number must be a whole number. `233.5`, a string, `null` and a missing key are `shape`.
- Each point must be on the glass: inside the circle of diameter 466 through the pixel centres. Corner
  points such as `(0,0)` are `range`. A swipe is checked at both ends. The line between two points of
  a circle stays inside it.
- `ms` outside its limit is `range`.
- The device never clamps. A silent clamp would hide a mistake in the script.

### What happens on the device

The touch task is the code that reads the CST9217. While a gesture plays, the task uses the injected
sample **in place of the glass** on each pass (every 4 ms while down). Everything after that is the
real path: the press count, the activity timer, the display-wake rule, `habitat_touch()` and so every
screen's handlers.

- If the display is asleep, an injected press wakes it and is swallowed, like a finger. Send a second
  tap.
- A real finger on the glass at the same time is overridden for the length of the gesture.
- When nothing is injected, the cost for the real touch path is one atomic load per pass.
- One gesture at a time. A second request while one plays is `busy`.

### Reply

When the gesture is over (the release was delivered), the device sends:

```json
{"t":"debug.done","id":3,"cmd":"swipe"}
```

The reply is sent from the cable task's tick, so it is up to about 100 ms later than the release. Take
a screenshot after `debug.done`. Allow another 50 to 100 ms for the UI to draw the result.

On failure:

```json
{"t":"debug.error","id":3,"cmd":"swipe","reason":"busy"}
```

| `reason` | Meaning |
| --- | --- |
| `shape` / `range` | The request is malformed or off the face (see Validation). Nothing was played. |
| `busy` | A gesture is already playing. |
| `touch` | The touch controller is not up, or the gesture was lost (the touch task did not take it within 1 s, or did not finish within its length plus 2 s). |

## How the screenshot is made, and why

The panel is write-only: there is no way to read the picture back from it. The renderer
(`display_habitat.c`) does not keep a framebuffer either. It keeps two `ht_scene_t` structures (a list
of text runs, boxes and sprites), finds what changed, and draws only those rows into two 24-line
internal DMA buffers.

Three ways to get a picture were considered:

| Way | Verdict |
| --- | --- |
| Tee every strip the renderer sends into a PSRAM copy | Needs a permanent 434 KB buffer, and every frame pays a copy that nobody asked for. Strips are partial updates, so the copy would also need a first full paint. **Rejected.** |
| Read the panel back over QSPI | This driver only writes to the CO5300. There is no read path. **Not available.** |
| **Draw the current scene again into a buffer, only when asked** | The scene already holds the exact picture. `ht_raster()` is the same call `paint()` makes for the panel. **Chosen.** |

**What is done.** `debug.shot` allocates 434,312 bytes in PSRAM, asks the render task to rasterize
`scenes[front]` (the scene the panel is showing) into it, 24 rows at a time, and sends it. The render
task does the work, between two frames, because `ht_raster()` owns two caches that only one renderer may
use, and because that scene is stable only there.

**What this is not.** It is the picture the renderer *meant* to show, not a readback of the glass. It does
not include the panel's own brightness setting (register `0x51`). A fault in the incremental update
path (a damage rectangle that misses) would not show, because the screenshot is a full redraw. The host
tests (`test_terminal`, `test_damage_*`) check that incremental and full redraw agree.

### Cost

| | Value |
| --- | --- |
| PSRAM | 434,312 bytes, for the length of the transfer only. Freed on every exit. |
| Refused when | The largest free PSRAM block is under 434,312 bytes, or the free total is under 434,312 + 512 KiB. The 4.5 MB free on this board leaves a wide margin. |
| Internal RAM | A 1,028-byte static chunk buffer and a few words. No new task, no new stack. |
| Renderer stall | One extra full-face raster, on the render task. The device logs `screenshot raster N us` each time. The renderer watchdog (4 s) is kept alive between strips. |
| Cable reader | Busy while the picture is sent: 425 frames of 1,036 bytes. The 32 KiB receive ring holds what the host sends meanwhile. Other senders (`hello`, logs) can still write between chunks, since the transmit lock is taken per frame. |
| Flash | 5,728 bytes (measured). |
| Time | **Not yet measured on the device.** The log line `shot id=N: ... capture X ms, send Y ms, crc32 ...` gives both. USB full speed carries about 1 MB/s at best, so the send is at least about 0.45 s. |

Until a `debug.shot` arrives, nothing is allocated except two semaphores made at display start, and
the renderer does no extra work.

## Tests

Host tests, no board (`make device-test`, or `test/run.sh`):

| Test | Covers |
| --- | --- |
| `test_debug_cmd.c` | CRC-32 against the textbook form and the `123456789` check value; the 425-chunk round trip (headers, last chunk, checksum, loss detected); the memory rule; the round-face rule (area, symmetry, corners); request validation limits; the gesture sampler; the cable-task/touch-task hand-off, including real threads. |
| `test_debug_cmd_json.c` | Reading every request shape; the exact bytes of the replies; allocation failure at every point leaves no partial reply and no leak. Needs `IDF_PATH` (for cJSON), like `test_cable_features.c`. |
| `test_touch_inject.py` | The production touch task with an injected gesture: tap and swipe reach `habitat_touch()` like a finger; stale ACKs; wake swallow while asleep; dead controller; no effect when nothing is armed. |
| `test_cable_features.c` | The `debug` feature name. |

## Through the daemon

Only one process can own the dial's USB port. While the Harness daemon holds it, `dial_debug.py` cannot open
the port. The daemon offers the same four commands on its local HTTP API (127.0.0.1:18473, the port of every
other local route), so a script, an agent or `curl` can look at the face and touch it.

Source: `cli/src/cable/dialDebug.ts` (ask and wait, one request at a time per dial), `cli/src/cable/png.ts`,
`cli/src/lib/dialDebugHttp.ts` (the HTTP half), and the `dial_debug` request of `cli/src/services/devices.ts`.

### Switch

**Off by default.** Set the environment variable of the daemon, then restart it:

```
CABLE_DEBUG_API=true
```

Off, the four routes do not exist: they answer 404 exactly as an unknown route does. On, they still follow
the rules of every other local route: a loopback `Host`, no foreign `Origin`, the local credential
(`X-Harness-Local: <contents of the hook-credential file in the daemon's data directory>`) and the CSRF header
`X-Adapter-Local: 1`. The request is for a process on this computer only.

### Routes

All `POST`, JSON body.

| Route | Body | Answer |
| --- | --- | --- |
| `/api/dial/debug/shot` | optional `{"dial":"<mac or serial>"}` | `200 image/png`, the 466x466 face |
| `/api/dial/debug/tap` | `{"x":233,"y":233}` | `200 {"ok":true}` after the dial's `debug.done` |
| `/api/dial/debug/press` | `{"x":233,"y":233,"ms":700}` (`ms` 50..5000, default 700) | `200 {"ok":true}` |
| `/api/dial/debug/swipe` | `{"x1":380,"y1":233,"x2":80,"y2":233,"ms":250}` (`ms` 20..3000, default 250) | `200 {"ok":true}` |

Every route takes an optional `"dial"`: the dial's MAC (from its `hello`) or its USB serial, in any case.
Without it the request goes to the first dial that is connected. A name that matches no dial is a 404; the
request is never sent to another dial.

Every point is a whole number from 0 to 465. The daemon turns away anything else with a 400 before the dial
sees it. A point inside that square but off the round glass is the dial's to refuse (`range`).

### Errors

A failure is JSON, with the status below:

```json
{"error":"the dial refused: asleep","kind":"device","reason":"asleep"}
```

`error` is a sentence, `kind` is one word, and `reason` is the dial's own `debug.error.reason` when it gave one.

| Status | `kind` | Meaning |
| --- | --- | --- |
| 400 | `bad_request` | The body is not JSON or a number is missing or out of range. Nothing was sent. |
| 403 | | No `X-Adapter-Local` header, or a `Host` that is not loopback. |
| 401 | | No local credential. |
| 404 | `no_dial` | No dial is connected, or none has that MAC or serial. |
| 409 | `busy` | A debug request for that dial is already waiting, or the dial answered `busy`. |
| 502 | `device` | The dial refused (`reason`: `asleep`, `memory`, `shape`, `range`, `touch`, ...) or sent a `begin` that is not a picture. |
| 502 | `incomplete` | A screenshot lost a chunk, repeated one, or has the wrong number of bytes. No image is returned. |
| 502 | `checksum` | The CRC-32 over the pixels does not match the dial's `end`. No image is returned. |
| 502 | `gone` | The link dropped, or the write failed. |
| 503 | `unavailable` | The devices service did not answer. |
| 504 | `timeout` | The dial did not finish: 20 s for a picture, 8 s for a tap, the gesture's `ms` plus 5 s otherwise. |

A picture is returned only when it passes the four rules of "How the host detects loss" above, including the
CRC-32. An `asleep` dial needs a tap first, then a second request.

### One request at a time

The daemon keeps one debug request in flight for each dial. A second request while the first waits is a 409
at once, and is not sent. The dial's normal traffic (agent lists, pings, a voice turn) goes on while a picture
is on its way. If the link drops the request ends as `gone`; a late reply is ignored by `id`.

### Example

```
curl -s -X POST http://127.0.0.1:18473/api/dial/debug/tap \
  -H "X-Harness-Local: $(cat "$DATA_DIR/hook-credential")" -H "X-Adapter-Local: 1" \
  -d '{"x":233,"y":233}'
curl -s -X POST http://127.0.0.1:18473/api/dial/debug/shot \
  -H "X-Harness-Local: $(cat "$DATA_DIR/hook-credential")" -H "X-Adapter-Local: 1" -o face.png
```

Allow 50 to 100 ms between a `done` and the next `shot`, as above, for the screen to draw the result.
`$DATA_DIR` is the daemon's data directory (`ADAPTER_DATA_DIR`).

Not measured on a dial yet: the end-to-end time of a picture through the daemon, and the way the daemon's
other traffic behaves during the 425 chunks. The tests drive a loopback port, not the board.
