// The RTC record of the automatic mode switch (task W15): the focused agent and the count of switches in a row, carried across the software reset
// of a switch. The layout, the checksum and the rules for trusting one are mode_switch.c's (ms_record_*); this file owns the variable.
//
// The variable is RTC_NOINIT, the memory last_words.c keeps its ring in: not touched by the bootloader or the start-up code, so it holds across
// esp_restart(); a power cut clears it to nothing in particular. So the record is trusted only after a SOFTWARE reset, and after any other reset
// the boot sees zeros and zeroes the record: a power cut gives zero, never garbage, and a later software reset cannot find old contents.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_auto.h"
#include "mode_switch.h"

// At boot, once. `reset_is_software` is esp_reset_reason() == ESP_RST_SW. Fills `view` (zeros when the record is not trusted) and returns whether
// it was. Then re-seals the record with the SAME flap count and NO focus: the focus is restored once, and a later restart that is not a mode switch
// (a firmware update, the factory reset) must not move the carousel to an agent from an earlier switch; the count stays, so a boot loop of
// switches keeps counting toward MS_FLAP_CAP.
bool mode_record_boot(bool reset_is_software, ms_record_view_t *view);

// Seal the focus (NULL or "" for none) and the flap count of the switch about to happen. Called just before esp_restart().
void mode_record_save(const char *focus, uint8_t flaps);

// ms_out_t.flaps_cleared: 10 minutes of uptime in one mode. Re-seal with a zero count and no focus.
void mode_record_clear_flaps(void);

#ifdef MODE_RECORD_HOST
// Host tests only: the record itself, to scribble over (a power cut) or to inspect.
ms_record_t *mode_record_raw_for_test(void);
#endif

// ── the crash record (panic loop, user decision 2026-10-31; rules in mode_switch.h) ──────────────────────────────────────────────────────────
// A sibling RTC_NOINIT variable with its own checksum. Unlike the record above it is believed after EVERY kind of reset but a power-on, because
// it exists to count the abnormal ones. A power cut that loses it loses the count; that is the safe side.

// At boot, once, before the boot decision. Returns whether this boot is the third (or later) crash in a row in Wi-Fi mode and sets *crashes to the
// count after this reset. A power-on zeroes the record. Writes nothing otherwise: seal after the mode is known.
bool mode_record_crash_boot(ms_reset_t cls, uint8_t *crashes);
// Seal the count and the mode that is running (or about to start). Called after the boot decision, and again with the mode that really started.
void mode_record_crash_seal(uint8_t crashes, ms_mode_t running);
// MS_CRASH_CLEAR_MS of uptime in Wi-Fi mode: the count goes to zero, the mode stays.
void mode_record_crash_clear(void);

#ifdef MODE_RECORD_HOST
ms_crash_t *mode_record_crash_raw_for_test(void);
#endif

// ── the link_auto record (wifi-first work: "Wi-Fi first, else phone"; link_auto.h) ──────────────────────────────────────────────────────────
// A third RTC_NOINIT variable, 12 bytes: which radio this boot picked, the returns to Wi-Fi used, the automatic reboots used. Trusted after a SOFTWARE
// reset only (and with a good checksum and values in range); ANY other reset zeroes it and seals it again at once (the PMIC power-off keeps the RTC
// memory alive, so an old pick must not be believed by a later software reset). It is read in every boot but acted on only when the stored wl_mode is 3.

// At boot, once. True and the pick when trusted; otherwise false, `view` is zeros and the record is zeroed and sealed (so a later software reset reads Wi-Fi).
bool mode_record_la_boot(bool reset_is_software, la_view_t *view);
// Seal `v`: the record of the boot about to start (just before esp_restart() of an automatic edge), or the current state after a clear.
void mode_record_la_save(const la_view_t *v);
// A USB boot, or the automatic mode going off: zero, sealed.
void mode_record_la_clear(void);

#ifdef MODE_RECORD_HOST
la_rec_t *mode_record_la_raw_for_test(void);
#endif
