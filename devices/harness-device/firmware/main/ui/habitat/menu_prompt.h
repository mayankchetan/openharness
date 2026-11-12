// The menu prompt (ui_habitat.c): "Hold BOOT 2 s for menu". A pull up from the bottom edge of the main face opens it; it asks for a 2 s hold of the BOOT key
// before the controls list opens, so a stray swipe never lands in a menu. Pure logic: no IDF include, no clock of its own, no I/O, so a host test
// (test_menu_prompt.c) runs every rule. ui_habitat.c feeds it the BOOT level (ptt.c polls the pin every 100 ms) and the time.
//
//   open       a pull up from the bottom edge (ht_menu_open). BOOT already down at that moment is not counted: the key has to go up and down again.
//   confirm    BOOT down for HT_MENU_HOLD_MS with the prompt up: ht_menu_boot says true once, the prompt is closed and the release of that same press
//              is swallowed (ht_menu_swallowing): without it the release would be a BOOT tap, which goes home and would close the menu just opened.
//   early      BOOT released before that: nothing happens, the prompt stays, the bar empties.
//   dismiss    a swipe down (ht_menu_close), or HT_MENU_TIMEOUT_MS without an action. A BOOT press counts as an action and restarts the 10 s.
//   voice      while the prompt is up BOOT starts no capture (ui_habitat.c boot_voice refuses), and a BOOT tap does nothing (ht_menu_eats_tap).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HT_MENU_HOLD_MS     2000u
#define HT_MENU_TIMEOUT_MS  10000u

typedef struct {
    bool open;
    bool armed;        // BOOT has been seen up since the prompt opened: only then does a press count
    bool down;         // BOOT is down and counted
    bool swallow;      // the confirming press is still down: its release is not a tap
    uint32_t since_ms; // the last action (open, press, release): the 10 s run from here
    uint32_t down_ms;  // when the counted press began
} ht_menu_t;

void ht_menu_open(ht_menu_t *m, uint32_t now_ms, bool boot_down_now);
void ht_menu_close(ht_menu_t *m);
// One BOOT sample (call it every poll, with the prompt up or not). True exactly once: the hold completed. Then the prompt is closed and swallowing is on
// until the sample that shows BOOT up.
bool ht_menu_boot(ht_menu_t *m, uint32_t now_ms, bool down);
// True when the 10 s ran out (the prompt is closed by this call). Never while BOOT is held.
bool ht_menu_tick(ht_menu_t *m, uint32_t now_ms);
// How far the hold has come, 0..100 (0 when BOOT is up).
int ht_menu_progress(const ht_menu_t *m, uint32_t now_ms);
// A BOOT tap is not acted on: while the prompt is up, and for the release of the press that opened the menu.
bool ht_menu_eats_tap(const ht_menu_t *m);
// Milliseconds until the machine next needs a look (the next redraw of the bar while BOOT is held, else the timeout); 0 when idle.
uint32_t ht_menu_next_ms(const ht_menu_t *m, uint32_t now_ms);
