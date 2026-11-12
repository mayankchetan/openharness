// The logic behind the Settings screen's rows (ui_habitat.c): the timer and link cycles, Night mode and Battery saver. Pure: no IDF include, no clock, no
// NVS, so a host test (test_prefs_logic.c) runs every rule. ui_habitat.c reads the world, calls one of these, and carries out the answer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── Screen-off timer ──────────────────────────────────────────────────────────────────────────────────────────────────────────────
// A stored index. AUTO is the value of a dial nobody has touched: the screen goes off after 300 s on external power and 60 s on the battery, as it always did
// (power_policy.h). The first tap leaves AUTO for good; the cycle is 30 s, 1 min, 2 min, 5 min, and round again.
enum { HT_TIMER_AUTO = 0, HT_TIMER_30S, HT_TIMER_1M, HT_TIMER_2M, HT_TIMER_5M, HT_TIMER_COUNT };
uint8_t ht_timer_next(uint8_t timer);          // AUTO or a value that is no index gives 30 s; 5 min gives 30 s
uint32_t ht_timer_ms(uint8_t timer);           // 0 for AUTO (and for a value that is no index): the policy's own numbers
const char *ht_timer_label(uint8_t timer);     // "Auto", "30 s", "1 min", "2 min", "5 min"

// ── Unplugged link ────────────────────────────────────────────────────────────────────────────────────────────────────────────
// The numbers are the stored wl_mode: 1 Wi-Fi, 2 phone, 3 auto (Wi-Fi first, then the phone); 0 is "USB only". The cycle is Auto, Wi-Fi, Phone, Auto.
// `have_wifi` and `have_phone`: the settings of that radio are whole (config_net_info). `ble_image`: this image has the phone link at all (DEVICE_BLE_LINK);
// without it the phone and auto are not offered. A mode that is not on offer is skipped. Returns the next mode, or -1 when there is nothing else to go to.
int ht_link_next(int current, bool have_wifi, bool have_phone, bool ble_image);
bool ht_link_offered(int mode, bool have_wifi, bool have_phone, bool ble_image);
const char *ht_link_label(int mode);           // "USB only", "Wi-Fi", "Phone", "Auto"; "?" for anything else
// How many of the three are on offer: the row is shown only with two or more.
int ht_link_options(bool have_wifi, bool have_phone, bool ble_image);

// ── Night mode and Battery saver ──────────────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    bool night;             // Night mode is on
    bool saver;             // Battery saver is on
    uint8_t night_bright;   // the brightness (percent) Night mode took over from; valid while `night`
    bool night_muted;       // the muted flag it took over from; valid while `night`
    uint8_t timer;          // the user's own screen-off timer (HT_TIMER_*); Battery saver overrides it while on, and gives it back
} ht_prefs_t;
#define HT_NIGHT_BRIGHT 8        // Night mode's brightness: the panel's floor
#define HT_SAVER_BRIGHT_CAP 30   // Battery saver: the panel never goes above this percent (the user's own level is kept)
#define HT_SAVER_TIMER_MS 30000u // Battery saver: the screen-off time
#define HT_SAVER_PARK_MIN 2      // Battery saver: minutes of dark battery time before the Wi-Fi radio parks (5 without it)
#define HT_PARK_MIN 5

// Night mode on or off. `*bright` and `*muted` are the dial's levels now and are replaced by the levels to apply: on, 8 percent and muted, the old pair kept
// in `p`; off, the pair that was kept. Returns true when the levels changed.
void ht_night_toggle(ht_prefs_t *p, int *bright, bool *muted);
// The user moved the brightness arc while Night mode was on: Night mode goes off and keeps the level the user chose; the sound goes back to what it was.
// Returns true if Night mode was on (and so `*muted` may have changed).
bool ht_night_user_moved(ht_prefs_t *p, bool *muted);

// The cap on the panel (percent, 100 = none), the screen-off time in ms (0 = the policy's own), and the Wi-Fi park minutes the settings ask for.
int ht_panel_cap(const ht_prefs_t *p);
uint32_t ht_screen_off_ms(const ht_prefs_t *p);
int ht_park_minutes(const ht_prefs_t *p);
