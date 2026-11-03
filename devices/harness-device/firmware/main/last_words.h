// The last log lines before a reset, and the reason for it.
//
// A dial that panics, trips a watchdog or browns out reboots in under a second and greets the daemon as
// if nothing happened — from the desk it looks like an unplug. This keeps a small ring of the most recent
// log lines in RTC memory that survives a soft reset, and on the next boot, if the reset was not a clean
// one, replays them over the cable once a daemon is listening.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// At boot, before anything logs: read the reset reason, keep the previous boot's ring if the reset was
// abnormal, then arm the ring for this boot.
void last_words_boot(void);

// Append one formatted log line. Called from the log sink; cheap, spinlocked, never blocks.
void last_words_add(const char *line, size_t len);

// Once a daemon is listening (session up): log the reset reason, and the previous boot's last lines if
// that reset was a crash. Logs once per boot.
void last_words_report(void);

// Lines the firmware prints at boot are lost to the dial log when the daemon opens the port later (about 4.4 s after boot; 2026-11-02: the AXP2101 rail line
// and the last power-off line were missing on DIAG7). A module that prints such a line at boot registers a function here, once, and the function prints it
// again: last_words_report() runs every registered function once, right after the reset reason (and the sleep breadcrumb), before the replay of the
// previous boot's log. At most 4 functions; the same function twice counts once; a full table refuses quietly. The functions run on the task that reports
// (the cable session), so they must only format and log what they saved at boot, and read no bus.
void last_words_on_report(void (*fn)(void));

// Say WHY the reset about to happen is happening (the automatic mode switch passes "mode switch", task W15), so the next boot's report reads
// "reset reason: software (esp_restart): mode switch" and is not taken for a fault. Kept in RTC memory with a checksum; believed only after a
// software reset, and used once.
void last_words_note_reason(const char *reason);

// THE SLEEP BREADCRUMB (DEVICE_BATTERY_SLEEP only; the 2026-11-01 watchdog hang on dial :50 left no trace of where the dial was). The ptt task writes one
// record in RTC memory every wake (every 100 ms awake, every 250 ms in S2): where the power policy and the render task were, and what held the chip.
// After a panic or a watchdog reset the next boot reports the last one in one log line (last_words_report), with the entries the task watchdog said had
// starved (esp_task_wdt_isr_user_handler, defined here under the same flag). One struct copy and a checksum per call; nothing else.
typedef struct {
    uint32_t tick_ms;    // esp_timer milliseconds at the wake
    uint32_t sleeps;     // light sleeps since boot (power_sleep_sleeps())
    uint32_t holds;      // PP_HOLD_* bits at the wake
    uint8_t  want;       // policy state the ptt task asked for (0 S0, 1 S1, 2 S2)
    uint8_t  applied;    // state the render task has applied
    uint8_t  in_s2;      // the S2 steps are applied (or being applied)
    uint8_t  stage;      // the render task's stage (display_habitat.c RENDER_*, display_render_stage())
} last_words_crumb_t;
void last_words_crumb(const last_words_crumb_t *c);
