// Compile-time switches for the S2 power-down work (2026-11-02): what the dial turns off when the chip may light-sleep with the screen dark.
// Each one is its own switch so a bench run can compare two images that differ in one thing. All of them are 0 unless the image is
// a DEVICE_BATTERY_SLEEP image, and in that image main/CMakeLists.txt sets each to 1 unless the build line says otherwise:
//   idf.py -DDEVICE_BATTERY_SLEEP=1 ... -DS2_AMP_OFF=0 build        (an image with that one switch off)
// Plan and measurements: workbench briefs/research/2026-11-02-dial-battery-drain.md, report 2026-11-02-s2-powerdown-build.md.
//
//   S2_AMP_OFF      GPIO46 (BSP_PA_IO, the NS4150B amplifier enable) is driven low when the audio clocks stop for S2 (audio_capture.c).
//   S2_CODEC_OFF    the ES7210 and ES8311 codecs are put in the driver's own stopped state when the audio clocks stop for S2 (audio_capture.c).
//   S2_PANEL_SLPIN  the CO5300 panel gets DISPOFF and DCS sleep-in (0x10) at S2 entry, sleep-out (0x11) at S2 exit (display_habitat.c, power_sleep.c).
#pragma once

#ifndef DEVICE_BATTERY_SLEEP
#undef S2_AMP_OFF
#undef S2_CODEC_OFF
#undef S2_PANEL_SLPIN
#endif

#ifndef S2_AMP_OFF
#define S2_AMP_OFF 0
#endif

#ifndef S2_CODEC_OFF
#define S2_CODEC_OFF 0
#endif

#ifndef S2_PANEL_SLPIN
#define S2_PANEL_SLPIN 0
#endif
