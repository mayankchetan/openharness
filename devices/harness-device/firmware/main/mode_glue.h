// The automatic mode switch on the dial (task W15): the glue that reads the world and acts on the pure module's answers (mode_switch.h). The
// decisions are mode_switch.c's and mode_glue_core.c's, run on the host in test_mode_switch.c and test_mode_glue.c; this file holds the calls into
// the IDF, the UI and the drivers, and has no logic of its own beyond reading and acting.
//
//   app_main.c:   const ms_mode_t boot = mode_glue_boot();                      // NVS, the SOF flag, the RTC record -> which mode to start
//                 const bool wifi = boot == MS_MODE_WIFI && net_link_start();    // the radio may not come up: then it is USB, in this boot
//                 cable_client_start_with(wifi ? net_link_xport() : cable_xport_usb());
//                 mode_glue_restore_focus();                                      // the agent the dial showed before the switch
//                 mode_glue_start(wifi ? MS_MODE_WIFI : MS_MODE_USB);             // the 250 ms task that decides the next switch
//
// The task "mode_sw" has an INTERNAL stack on purpose: it writes NVS (boot_usb) and installs and removes the USB driver, and flash operations stay
// off PSRAM stacks (task_util.h). It is the only caller of the probe.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mode_switch.h"

// Decide which mode this boot starts in. Once, after config_store_init and after the factory-reset countdown (which can wipe the values it reads).
// Reads the RTC record (trusted only after a software reset), waits until MS_SOF_SETTLE_MS of uptime if it has to, reads the SOF flag once, runs
// ms_boot_decide, and stores boot_usb when the choice changes it. Returns MS_MODE_USB or MS_MODE_WIFI, never BLE.
ms_mode_t mode_glue_boot(void);

// Put the carousel back on the agent the dial showed before an automatic switch (ui_focus_project; an agent that is not on the carousel yet is
// held until the roster arrives). A no-op after any other kind of boot.
void mode_glue_restore_focus(void);

// Start the task that steps the mode switch. `running` is the mode that really started. A dial whose task cannot be created simply never switches
// by itself (logged); it stays in the mode it booted in.
void mode_glue_start(ms_mode_t running);

// Settings changed (wl_mode or stay_wl, task W16): re-read them. Safe from any task.
void mode_glue_settings_changed(void);

// "Never mid-action" (MS section 3). The mask of what the user is in the middle of, built from the UI states, an update or upload in flight and the
// blocking model list; and the verdict with the asleep rule applied (a sleeping dial switches silently, except for an update or an upload).
uint32_t mode_switch_busy(void);
bool mode_switch_blocked(void);
// The dial's Keep Wi-Fi setting (stay_wl), cached: written at start and after every reload of the settings, readable from any task (the UI's caption for a
// refused key reads it on the render task, which must not read NVS).
bool mode_glue_keep_wifi(void);
