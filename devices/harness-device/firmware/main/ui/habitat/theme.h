#pragma once

// One charcoal surface, two neutral inks, one companion/action accent.
// Canvas matches the desktop terminal pane and Cmd N / Cmd P surfaces.
// All colors are RGB565-representable at full brightness. Conversion and the
// existing saved brightness setting remain at the UI boundary; no theme heap.
#define HT_THEME_CANVAS    0x181818u
// The Focus skin's canvas. Its design (mockup/newdesign.html, every screen) is on black, not on the
// terminal's charcoal — Focus is the pre-habitat layout and brings its ground with it.
#define HT_THEME_FOCUS_CANVAS 0x000000u
#define HT_THEME_TEXT      0xefe7deu
#define HT_THEME_SECONDARY 0xada6adu
#ifdef DEVICE_HABITAT_ORANGE
#define HT_THEME_ACCENT    0xff6d00u
#else
#define HT_THEME_ACCENT    0xc6aaefu
#endif
#define HT_THEME_SELECTION 0x392c4au
#define HT_THEME_ERROR     0xe7a6adu
// Desktop activityColor() / darkTerminalTheme ANSI status colors.
//
// The inbox status mark was the only coloured thing on the glass until the Focus skin, which reads a
// live status in HT_THEME_DONE and draws each engine in its own ink (focus.c, ENGINE_INK). On the
// creature skins the rule is unchanged: pane name, message and navigation stay neutral.
// The voice green: the recording waveform and the microphone that starts it.
//
// It is a pure channel — brighter and harder than HT_THEME_DONE, which is the terminal's "finished"
// green and is what a status line uses. The old firmware kept the two apart for the same reason: the
// meter is the one thing on the glass that is supposed to shout, and a status line is not.
#define HT_THEME_VOICE     0x00ff2fu
#define HT_THEME_DONE      0x0dbc79u
#define HT_THEME_QUESTION  0xe5e510u
#define HT_THEME_FAILED    0xcd3131u
// The battery rim (battery_rim.h): amber at 20 percent and below (red is HT_THEME_FAILED, at 10 and below), the track that marks where
// 100 percent ends (faint, but readable on the glass), and the percent number shown for 3 s after a wake: its backing is a near
// black that differs from the ground (a text run paints its cells only then).
#define HT_THEME_WARN          0xf0a020u
#define HT_THEME_RIM_TRACK     0x484548u
#define HT_THEME_RIM_TEXT_BG   0x000400u
