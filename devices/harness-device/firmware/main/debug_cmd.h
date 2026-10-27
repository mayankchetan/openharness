// The logic behind the host's debug commands — a screenshot of the face and injected touch — kept free of
// ESP-IDF so test/test_debug_cmd.c runs it on a laptop, the way cable_frame.c is checked. What cannot be
// checked there (the capture itself, the USB writes, the touch task's hook) lives in cable_debug.c,
// display_habitat.c and touch_habitat.c and is only as thick as it has to be.
//
// Frame shapes and the reasoning: docs/DEBUG_COMMANDS.md. Compiled in by default; the build option
// -DDEVICE_NO_DEBUG_COMMANDS=1 removes all of it (main/CMakeLists.txt).
//
// WHY THIS EXISTS. The panel is write-only and the board has one USB port, so the only way to see what a
// screen looks like was a camera. The host asks for the picture and sends the finger instead.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui/habitat/terminal.h"   // HT_WIDTH, HT_HEIGHT — the face is named once

typedef struct cJSON cJSON;

// ── errors ──────────────────────────────────────────────────────────────────────────────────────────
// One vocabulary for the parser, the capture and the injector, so every refusal reaches the host as a
// `debug.error` with a reason it can act on rather than as silence.
typedef enum {
    DBG_OK = 0,
    DBG_NOT_DEBUG,   // parser only: not a debug.* message, the caller keeps looking
    DBG_E_SHAPE,     // a field is missing, not a number, not an integer, or not the right type
    DBG_E_RANGE,     // well-formed but off the face, or a duration outside what makes sense
    DBG_E_BUSY,      // a capture or a gesture is already in flight
    DBG_E_MEMORY,    // not enough PSRAM for the picture (capture refuses rather than squeeze the heap)
    DBG_E_ASLEEP,    // the panel is off or has not painted yet, so there is no picture to take
    DBG_E_TIMEOUT,   // the renderer did not answer in time
    DBG_E_TOUCH,     // the touch controller is not up, so there is no task to inject into
    DBG_E_IO,        // the host stopped reading mid-transfer
} dbg_err_t;

// The `reason` string on the wire. Never NULL.
const char *dbg_err_name(dbg_err_t err);

// ── the screenshot ──────────────────────────────────────────────────────────────────────────────────
// Raw pixels, row-major from the top left, exactly as the renderer produces them for this panel: RGB565
// with the high byte first. No row padding, so the byte count is width * height * 2.
#define DBG_SHOT_FMT      "rgb565be"
#define DBG_SHOT_BYTES    ((uint32_t)HT_WIDTH * HT_HEIGHT * 2)

// Pixel bytes per frame. 1 KiB is the PCM chunk size (CABLE_VOICE_CHUNK) and the reason is the same: the
// transmit lock is held for one frame at a time, and a log line only waits 5 ms for it. Bigger chunks
// would buy a little speed and cost every other sender a longer wait.
#define DBG_SHOT_CHUNK    1024

// Binary chunk payload (frame type CABLE_TYPE_SHOT): id:u16 | index:u16 | pixels, little-endian.
#define DBG_SHOT_HEADER   4

// What PSRAM must still hold after the picture is taken. The picture is 424 KiB and temporary; this is
// the margin that keeps a screenshot from being the allocation that starves something else.
#define DBG_SHOT_RESERVE  (512u * 1024u)

// CRC-32/ISO-HDLC — the one zlib, PNG and Python's zlib.crc32 compute. Running form: pass the previous
// result back in to continue, start from 0.
uint32_t dbg_crc32(uint32_t crc, const uint8_t *data, size_t length);

// How many chunks `total` bytes take at `chunk` bytes each. 0 when either is 0.
uint32_t dbg_shot_chunks(uint32_t total, uint32_t chunk);

// Whether PSRAM can take the picture: one block big enough, and DBG_SHOT_RESERVE left over in all.
bool dbg_shot_memory_ok(size_t largest_block, size_t free_total);

// Write chunk `index` of `pixels` (`total` bytes) into `out`: the 4-byte header, then up to `chunk` bytes.
// Returns the payload length, or 0 when `index` is past the end or `cap` is too small.
size_t dbg_shot_chunk_encode(uint16_t id, uint32_t index, const uint8_t *pixels, uint32_t total,
                             uint32_t chunk, uint8_t *out, size_t cap);

// ── injected touch ──────────────────────────────────────────────────────────────────────────────────
typedef enum { DBG_TAP, DBG_PRESS, DBG_SWIPE } dbg_kind_t;

// A tap is a press of a fixed short length; a press holds for `ms` (a long-press is one over 650 ms,
// where the pet's hold begins); a swipe moves in a straight line over `ms`.
#define DBG_TAP_MS        60
#define DBG_PRESS_MIN_MS  50
#define DBG_PRESS_MAX_MS  5000
#define DBG_SWIPE_MIN_MS  20
#define DBG_SWIPE_MAX_MS  3000

// After the last moving sample the finger stays on the end point this long before it lifts, so the end
// position is always delivered as a contact and never only as a release.
#define DBG_END_HOLD_MS   8

typedef struct {
    dbg_kind_t kind;
    int16_t x1, y1, x2, y2;   // a tap or press has the two ends equal
    uint16_t ms;
} dbg_gesture_t;

// Whether (x, y) is on the glass: the disc of diameter HT_WIDTH through the pixel centres. The corners of
// the square buffer are not touchable and the controller never reports them.
bool dbg_point_ok(int x, int y);

// Build a gesture from numbers as they arrive off the wire (a missing field is NAN). Whole numbers only:
// 233.5 is a mistake in the script, not a request to round. DBG_E_SHAPE for a missing or non-integer
// value, DBG_E_RANGE for a point off the face or a duration outside its limits.
dbg_err_t dbg_tap_make(double x, double y, dbg_gesture_t *out);
dbg_err_t dbg_press_make(double x, double y, double ms, dbg_gesture_t *out);
dbg_err_t dbg_swipe_make(double x1, double y1, double x2, double y2, double ms, dbg_gesture_t *out);

// Where the finger is `elapsed_ms` after the gesture began.
typedef struct {
    bool down;      // false only on the closing sample
    bool done;      // the closing sample: the finger lifts at (x, y) and the gesture is over
    int16_t x, y;
} dbg_sample_t;
dbg_sample_t dbg_gesture_at(const dbg_gesture_t *g, uint32_t elapsed_ms);

// ── the hand-off between the cable task and the touch task ──────────────────────────────────────────
// One gesture at a time. The cable task arms it and later collects the outcome; the touch task plays it
// out as if it were the glass. Each state has exactly one writer at a time, so a handful of atomics is
// the whole of the synchronisation.
//
//   cable task: arm  ->  [ARMED]  touch task: poll ->  [RUNNING]  poll ->  [DONE]  cable task: finish -> IDLE
//
// Unused, none of it costs the real touch path more than one relaxed load per pass.
bool dbg_inject_arm(const dbg_gesture_t *g, int64_t now_us);   // false: one is already in flight

// Touch task, once per pass. True when `out` replaces what the glass said for this pass.
bool dbg_inject_poll(int64_t now_us, dbg_sample_t *out);

// Touch task: the controller went away, so the gesture cannot finish.
void dbg_inject_abort(void);

// Cable task, from its tick. 0 = nothing finished, 1 = the gesture played out, -1 = it did not (the touch
// task never picked it up, was lost partway, or aborted). Also gives up on a gesture that has outlived
// its own length by a margin.
int dbg_inject_finish(int64_t now_us);

// ── the requests ────────────────────────────────────────────────────────────────────────────────────
typedef enum { DBG_CMD_NONE, DBG_CMD_SHOT, DBG_CMD_TAP, DBG_CMD_PRESS, DBG_CMD_SWIPE } dbg_cmd_t;

typedef struct {
    dbg_cmd_t cmd;
    uint16_t id;             // echoed in every reply; 0 when the host sent none
    dbg_gesture_t gesture;   // the touch commands
} dbg_request_t;

// Read a message `t` with body `p` (the message itself when it is flat). DBG_NOT_DEBUG when `t` is not one
// of ours; otherwise `out->cmd` and `out->id` are set as far as they could be read, so a refusal can
// still name the command and echo the id.
dbg_err_t dbg_request_parse(const char *t, const cJSON *p, dbg_request_t *out);

// ── the replies ─────────────────────────────────────────────────────────────────────────────────────
// Each returns a JSON object as text, to be freed with cJSON_free, or NULL on allocation failure.
const char *dbg_cmd_name(dbg_cmd_t cmd);
char *dbg_reply_error(dbg_cmd_t cmd, uint16_t id, dbg_err_t err);
char *dbg_reply_done(dbg_cmd_t cmd, uint16_t id);
char *dbg_reply_shot_begin(uint16_t id);
char *dbg_reply_shot_end(uint16_t id, uint32_t chunks, uint32_t crc);
