#pragma once
// The UI gallery (-DDEVICE_UI_GALLERY=1, ui_gallery.inc): start the tour task. Call after display_init(), ui_init() and
// the cable link are up. Never called in the DEVICE_UI_GALLERY_NO_DISPLAY image.
void ui_gallery_start(void);
