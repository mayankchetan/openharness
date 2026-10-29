// CST9217 capacitive touch → LVGL pointer indev (enables tileview swipe).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize I2C + CST9217 + register an LVGL pointer indev. Call after lv_init().
// Tolerant: logs and returns on failure (display still works, just no touch).
void touch_init(void);


// Monotonic generation bumped once for every physical touch press. Background rendering, voice, and
// programmatic display activity do not affect it, so callers can cheaply detect real user interaction.
uint32_t touch_activity_generation(void);

// What the heartbeat prints about touch (display.c, once a minute): whether the controller answers,
// how many presses the glass has seen, and how long ago the last one was. Cheap; no lock.
typedef struct {
    uint32_t presses;
    uint32_t last_press_ms_ago;
    uint32_t read_failures;
    uint32_t inferred_releases; // releases the chip never reported, declared from its silence
    bool     controller_ok;
    bool     held_now;         // a press has been down past the stuck threshold
} touch_stats_t;
void touch_stats(touch_stats_t *out);

#ifdef DEVICE_BATTERY_SLEEP
// Wake the touch task from its idle wait (the 5 s S2 poll, power_policy.h PP_TOUCH_POLL_MS): the sleep policy calls it from the screen-wake hook, so
// the first touch after a PWR wake is read at once and not at the end of the wait. Safe from any task; a no-op until the task exists.
void touch_wake_task(void);
// Battery sleep (power_sleep.c, render task): arm GPIO11 as the LOW-level light-sleep wake source, and put the pin back as the
// touch driver wants it (falling-edge interrupt) when the screen is awake again. Both are safe to repeat and do nothing on a dial
// without the CST9217. touch_sleep_arm() returns false (and arms nothing) if it cannot: another chip, no pin lock, controller not up. The wake runs the driver's own interrupt handler; no second one is installed.
bool touch_sleep_arm(void);
void touch_sleep_disarm(void);
// True while the wake is armed AND the controller is up. False after the driver was deleted (read errors) until it is reopened:
// touch_habitat.c re-arms the pin on reopen, so this turns true again by itself.
bool touch_sleep_armed(void);
// The esp_timer time of the first level interrupt since the last call (0: none, e.g. a PWR-key wake). The render task takes it when the
// full repaint after a wake is on the glass and logs the interrupt-to-frame time (gate G6 of the plan: 99 percent at most 60 ms).
int64_t touch_sleep_take_isr_us(void);
#endif

#ifdef HARNESS_DEBUG_COMMANDS
// Wake the touch task so it plays an injected gesture (dbg_inject_arm, debug_cmd.h) at once instead of
// at its next idle poll. The gesture itself is read by the task, in place of the glass, on every pass.
void touch_inject_wake(void);
#endif
