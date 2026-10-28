#pragma once
// The UI gallery (-DDEVICE_UI_GALLERY=1, ui_gallery.inc): start the tour task. Call after display_init(), ui_init() and
// the cable link are up. Never called in the DEVICE_UI_GALLERY_NO_DISPLAY image.
void ui_gallery_start(void);

// The tour task's stack (internal RAM, taken from the heap as soon as ui_gallery_start runs). The re-logged
// "RAM tag=gallery_ready" line of the display image includes it; GALLERY ui=<0|1> tour_stack_b=<n> says how much.
#define UI_GALLERY_TOUR_STACK 8192

// Log "RAM tag=gallery_ready int_free= ..." and a "GALLERY ui= tour_stack_b=" line after it. app_main.c defines it; the
// cable client calls it once the session is up and every 30 s, so the line reaches the daemon's dial log (the boot-time
// call happens before the session, when the log still goes to the raw console).
void gallery_ram_report(void);
