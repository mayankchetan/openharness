#pragma once
// THE BOOT KEY (GPIO0), as a state machine. Pure logic: no ESP-IDF include, no clock, no I/O. The ptt task reads the pin, calls bk_step()
// every poll and carries out the event it returns; test/test_boot_key.c compiles this file with a plain cc.
//
// Plan section 16 (user decisions D1, D3) and 16.1 item 3:
//   * A TAP (released before BK_HOLD_MS) interrupts the running turn, as the key always did, but now on the RELEASE, because until the key
//     is up nobody knows whether it is a tap or the start of a hold.
//   * A HOLD (down for BK_HOLD_MS) starts a voice capture, the same entry the touch gesture uses (ui_voice_start); the RELEASE sends it
//     (ui_voice_stop). If no capture can start (a turn is running, no link, not a talk screen) the hold is denied and its release is a tap.
//   * The screen toggle on a BOOT hold is gone (the PWR key does that).
//   * D1: where input is not allowed (battery, screen off) a press is ignored whole: no wake, no tap, no hold.
//   * A key already down when the task starts (held through power-on) is ignored until it is released.
// The cold-boot settings wipe (bkr_*) is its own small machine: BOOT held at power-on starts a countdown on the glass, the wipe happens
// only after BK_RESET_HOLD_MS of unbroken holding, and letting go cancels it.
#include <stdbool.h>
#include <stdint.h>

#define BK_HOLD_MS        400    // down this long: hold-to-talk (a tap is shorter; 300 ms thumbs are taps)
#define BK_STOP_WAIT_MS   3000   // released before the capture began: wait this long for it to begin, then give up
#define BK_RESET_HOLD_MS  5000   // BOOT held this long after power-on: wipe the settings (16.1 item 3)

typedef enum {
    BK_NONE = 0,
    BK_PRESS,        // the press edge, input allowed: the caller wakes a dark screen (external power only)
    BK_TAP,          // released as a tap (or a hold that was denied): interrupt the running turn
    BK_HOLD_START,   // the hold threshold passed and a capture can start: start it
    BK_HOLD_END,     // released after a hold that started: the capture is sent (bk_stop_poll says when it can be stopped)
} bk_event_t;

typedef struct {
    bool     down;
    bool     ignored;       // this press is not ours: D1, or held through power-on
    bool     hold_fired;    // BK_HOLD_START was returned for this press
    bool     talk_denied;   // the hold threshold passed and no capture could start
    uint32_t down_ms;
    bool     stop_pending;  // a hold ended: the capture has to be stopped once it is recording
    uint32_t stop_since_ms;
} bk_t;

// `pressed_now`: the pin reads low right now (a key held through power-on is ignored until it is released).
void bk_init(bk_t *k, bool pressed_now);
// One poll. `now_ms` is a free-running 32-bit millisecond clock (wrap is fine). `pressed` is the pin level (true = down). `allowed` is
// power_sleep_input_allowed() (D1). `can_talk` is asked once per press, when the hold threshold passes: true when a capture can start.
bk_event_t bk_step(bk_t *k, uint32_t now_ms, bool pressed, bool allowed, bool (*can_talk)(void));
// After a BK_HOLD_END: true exactly once, when `recording` is true (the capture has begun): the caller stops it. False while the capture
// is still starting; after BK_STOP_WAIT_MS it gives up (nothing started, nothing to stop). A new press cancels it.
bool bk_stop_poll(bk_t *k, uint32_t now_ms, bool recording);

// ---- the settings wipe at power-on ----
typedef enum { BKR_NONE = 0, BKR_COUNTING, BKR_CANCELLED, BKR_DONE } bkr_result_t;
typedef struct { bool started; bool cancelled; bool done; uint32_t since_ms; int last_secs; } bkr_t;
typedef struct {
    bkr_result_t result;
    int          seconds_left;   // 5 down to 1 while counting
    bool         changed;        // seconds_left differs from the last step: redraw the countdown
} bkr_out_t;
// True when BOOT is down at power-on and the countdown starts.
bool bkr_begin(bkr_t *r, uint32_t now_ms, bool pressed);
bkr_out_t bkr_step(bkr_t *r, uint32_t now_ms, bool pressed);
