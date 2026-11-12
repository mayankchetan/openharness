#include "prefs_logic.h"
#include <stddef.h>

static const uint32_t timer_ms[HT_TIMER_COUNT] = {0, 30000u, 60000u, 120000u, 300000u};
static const char *const timer_text[HT_TIMER_COUNT] = {"Auto", "30 s", "1 min", "2 min", "5 min"};

uint8_t ht_timer_next(uint8_t timer)
{
    if (timer == 0 || timer >= HT_TIMER_COUNT) return HT_TIMER_30S;
    return timer + 1 >= HT_TIMER_COUNT ? HT_TIMER_30S : (uint8_t)(timer + 1);
}
uint32_t ht_timer_ms(uint8_t timer) { return timer < HT_TIMER_COUNT ? timer_ms[timer] : 0; }
const char *ht_timer_label(uint8_t timer) { return timer < HT_TIMER_COUNT ? timer_text[timer] : timer_text[0]; }

static const int link_order[3] = {3, 1, 2};   // Auto, Wi-Fi, Phone

bool ht_link_offered(int mode, bool have_wifi, bool have_phone, bool ble_image)
{
    switch (mode) {
    case 1: return have_wifi;
    case 2: return ble_image && have_phone;
    case 3: return ble_image && have_wifi && have_phone;
    default: return false;
    }
}
int ht_link_options(bool have_wifi, bool have_phone, bool ble_image)
{
    int n = 0;
    for (int i = 0; i < 3; i++) n += ht_link_offered(link_order[i], have_wifi, have_phone, ble_image);
    return n;
}
int ht_link_next(int current, bool have_wifi, bool have_phone, bool ble_image)
{
    int at = -1;   // where `current` is in the cycle; USB only, or a byte that is no mode, is before the first
    for (int i = 0; i < 3; i++) if (link_order[i] == current) at = i;
    for (int step = 1; step <= 3; step++) {
        const int mode = link_order[(at + step + 3) % 3];
        if (mode != current && ht_link_offered(mode, have_wifi, have_phone, ble_image)) return mode;
    }
    return -1;
}
const char *ht_link_label(int mode)
{
    switch (mode) {
    case 0: return "USB only";
    case 1: return "Wi-Fi";
    case 2: return "Phone";
    case 3: return "Auto";
    default: return "?";
    }
}

void ht_night_toggle(ht_prefs_t *p, int *bright, bool *muted)
{
    if (!p->night) {
        p->night = true;
        p->night_bright = (uint8_t)(*bright < 0 ? 0 : *bright > 100 ? 100 : *bright);
        p->night_muted = *muted;
        *bright = HT_NIGHT_BRIGHT;
        *muted = true;
    } else {
        p->night = false;
        *bright = p->night_bright;
        *muted = p->night_muted;
    }
}
bool ht_night_user_moved(ht_prefs_t *p, bool *muted)
{
    if (!p->night) return false;
    p->night = false;
    *muted = p->night_muted;
    return true;
}

int ht_panel_cap(const ht_prefs_t *p) { return p->saver ? HT_SAVER_BRIGHT_CAP : 100; }
uint32_t ht_screen_off_ms(const ht_prefs_t *p) { return p->saver ? HT_SAVER_TIMER_MS : ht_timer_ms(p->timer); }
int ht_park_minutes(const ht_prefs_t *p) { return p->saver ? HT_SAVER_PARK_MIN : HT_PARK_MIN; }
