#pragma once
// DEVICE_SLEEP_PROFILE (default OFF; sleep_profile.c): the boot profile and a 500-cycle sleep/wake run.
//
// Boot: time from the start of the app to app_ready, the main task's stack high-water mark at app_ready and just before
// app_main returns, and the internal minimum free at app_ready ("slp: BOOT ..."; logged again when the run starts, because
// the boot-time lines go out before the daemon session exists). This decides whether the main stack (10,240 B) can shrink.
//
// Run, 20 s after boot (once the daemon session is up, up to 60 s): 500 cycles of sleep (display_lock, display_sleep, as the
// 300 s idle timer and the PWR key do), 1 s asleep, wake by a tap played through the real touch task (the press while asleep
// wakes the panel and is swallowed, as a finger's is), wait for the first full frame. One "SLP cycle=" line each, a summary
// every 50, "SLP DONE" at the end. Nothing is written to NVS.
#include <stdint.h>

void sleep_profile_app_ready(void);
void sleep_profile_main_end(void);
void sleep_profile_start(void);

// Timestamps (esp_timer, microseconds) and counters kept by display_habitat.c under the flag.
typedef struct {
    int64_t sleep_us;     // display_sleep() took effect
    int64_t off_us;       // the renderer switched the panel off
    int64_t wake_us;      // display_wake() took effect
    int64_t frame_us;     // the first full frame after a wake is on the glass (paint and DMA done)
    uint32_t off_seq;     // counts panel-off events
    uint32_t frame_seq;   // counts first-full-frames-after-wake
} display_slp_stamps_t;
void display_slp_stamps(display_slp_stamps_t *out);
