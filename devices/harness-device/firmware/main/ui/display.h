// Display bring-up: CO5300 AMOLED over QSPI + LVGL v9 port.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize the QSPI bus, CO5300 panel and LVGL, add the PSRAM object pool,
// allocate internal-DMA draw buffers, and spawn the handler task.
void display_init(void);
// Minimal display for the dedicated OTA boot mode (small draw buffers, no touch) — leaves internal RAM
// for the big WiFi RX buffers. Pair with ui_ota_boot_show / ui_ota_boot_pct.
void display_init_ota(void);

// Set the AMOLED brightness (CO5300 DCS 0x51): 0x00 = dimmest, 0xFF = max. Applied live.
void display_set_brightness(uint8_t level);

// Lock/unlock the LVGL mutex. ANY code that touches LVGL objects from outside
// the LVGL task (e.g. the WiFi/WS tasks via ui_screens.c) must hold this.
// `display_lock()` carries the caller's name so a wait past 2s can be logged with who is waiting and
// who is holding — see display_lock_at. The macro keeps every call site as it was.
void display_lock_at(const char *who);
#define display_lock() display_lock_at(__func__)
void display_unlock(void);

#ifdef DEVICE_BATTERY_SLEEP
// Stop (run = false) or start (true) the render stall guard, a 1 s periodic timer that would cap every light sleep at one second.
// Called by the sleep policy on the render task only, when the chip may sleep and when the screen comes back.
void display_render_guard(bool run);
// A second power hook beside display_set_power_cb(), for the sleep policy: cb(true) is called FIRST on every wake, from whichever
// task woke the screen (touch, ptt, render), cb(false) on every sleep. It must be quick and must not take display_lock.
void display_set_sleep_cb(void (*cb)(bool on));
#endif

// Idle battery-save: turn the AMOLED panel off / back on. `display_wake()` repaints the current
// screen and re-arms the idle timer. Call sites run on the LVGL task (idle check + touch wake).
void display_sleep(void);
void display_wake(void);
bool display_is_asleep(void);
// A counter that moves on every screen wake: display_wake() (from off) and a touch or key that brings a dimmed screen back to full
// brightness. The habitat UI compares it with the value it saw to show the battery percent for a few seconds after a wake.
uint32_t display_wake_seq(void);
// Register a power hook: cb(false) fires on sleep, cb(true) on wake. Used by the UI screen-lock to arm
// re-lock on sleep and show the unlock overlay on wake. Runs under display_lock (recursive).
void display_set_power_cb(void (*cb)(bool on));
// Reset the idle-off timer without a touch — ui_screens calls this while a voice turn is recording/
// uploading/processing (voice uses the PWR key, not the touchscreen, so the screen must not auto-off).
void display_bump_activity(void);

#ifdef HARNESS_DEBUG_COMMANDS
// Re-render the scene the panel is showing into `out` (HT_WIDTH x HT_HEIGHT RGB565, the renderer's own
// panel byte order, 2 bytes per pixel, no padding). `out` is the caller's — PSRAM, 434 KB — and nothing
// is allocated here. The work happens on the render task, between two frames, because that task owns the
// compositor's caches; this call blocks the caller (never the renderer) until it is done or `timeout_ms`
// passes. Returns DBG_OK, or DBG_E_ASLEEP (panel off / nothing painted yet), DBG_E_BUSY (a capture is
// already queued) or DBG_E_TIMEOUT. See docs/DEBUG_COMMANDS.md.
#include "debug_cmd.h"
dbg_err_t display_capture(uint16_t *out, uint32_t timeout_ms);
#endif
