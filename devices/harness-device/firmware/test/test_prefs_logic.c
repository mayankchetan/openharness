// The Settings screen's rules (ui/habitat/prefs_logic.c) and the screen policy that takes the user's time (power_policy.c pp_screen_level_cfg):
// the screen-off cycle, the Unplugged link cycle, Night mode's save and restore, and what Battery saver overrides.
#include "../main/ui/habitat/prefs_logic.h"
#include "../main/power_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    // ── Screen off: 30 s, 1 min, 2 min, 5 min, round again; the untouched dial ("Auto") leaves it on the first tap, and a stored byte that is no index too.
    assert(ht_timer_next(HT_TIMER_AUTO) == HT_TIMER_30S && ht_timer_next(HT_TIMER_30S) == HT_TIMER_1M && ht_timer_next(HT_TIMER_1M) == HT_TIMER_2M);
    assert(ht_timer_next(HT_TIMER_2M) == HT_TIMER_5M && ht_timer_next(HT_TIMER_5M) == HT_TIMER_30S && ht_timer_next(200) == HT_TIMER_30S);
    uint8_t t = HT_TIMER_AUTO;
    for (int i = 0; i < 12; i++) {
        t = ht_timer_next(t);
        assert(t != HT_TIMER_AUTO && "the cycle never goes back to Auto");
    }
    assert(ht_timer_ms(HT_TIMER_AUTO) == 0 && ht_timer_ms(HT_TIMER_30S) == 30000 && ht_timer_ms(HT_TIMER_1M) == 60000);
    assert(ht_timer_ms(HT_TIMER_2M) == 120000 && ht_timer_ms(HT_TIMER_5M) == 300000 && ht_timer_ms(77) == 0);
    assert(!strcmp(ht_timer_label(HT_TIMER_AUTO), "Auto") && !strcmp(ht_timer_label(HT_TIMER_30S), "30 s") && !strcmp(ht_timer_label(HT_TIMER_5M), "5 min"));

    // ── The policy with and without a user time. 0 is the old table, exactly: 300 s on a cable, dim at 30 s and off at 60 s on the battery.
    for (uint32_t idle = 0; idle <= 400000; idle += 500)
        for (int st = PP_S0; st <= PP_S2; st++)
            assert(pp_screen_level_cfg((pp_state_t)st, idle, 0) == pp_screen_level((pp_state_t)st, idle));
    // A user time moves dark, on a cable and on the battery; "after" is strict like the old one; dim is half of it on the battery and never on a cable.
    for (uint8_t timer = HT_TIMER_30S; timer <= HT_TIMER_5M; timer++) {
        const uint32_t off = ht_timer_ms(timer);
        assert(pp_screen_level_cfg(PP_S0, off, off) == PP_SCREEN_FULL && pp_screen_level_cfg(PP_S0, off + 1, off) == PP_SCREEN_OFF);
        assert(pp_screen_level_cfg(PP_S0, off - 1, off) == PP_SCREEN_FULL && "a cable never dims");
        assert(pp_screen_level_cfg(PP_S1, off / 2, off) == PP_SCREEN_FULL && pp_screen_level_cfg(PP_S1, off / 2 + 1, off) == PP_SCREEN_DIM);
        assert(pp_screen_level_cfg(PP_S1, off, off) == PP_SCREEN_DIM && pp_screen_level_cfg(PP_S1, off + 1, off) == PP_SCREEN_OFF);
        assert(pp_screen_level_cfg(PP_S2, off + 1, off) == PP_SCREEN_OFF);
    }
    // The 60 s on the battery is today's own time: choosing 1 min changes nothing there.
    for (uint32_t idle = 0; idle <= 90000; idle += 250)
        assert(pp_screen_level_cfg(PP_S1, idle, 60000) == pp_screen_level(PP_S1, idle));

    // ── Unplugged link: Auto -> Wi-Fi -> Phone -> Auto, with both radios set up in an image that has the phone link.
    assert(ht_link_next(3, true, true, true, true) == 1 && ht_link_next(1, true, true, true, true) == 2 && ht_link_next(2, true, true, true, true) == 3);
    assert(ht_link_next(0, true, true, true, true) == 3 && "USB only enters the cycle at Auto");
    assert(ht_link_next(99, true, true, true, true) == 3 && "a byte that is no mode too");
    // A mode that is not set up is skipped; with one mode only there is nothing to go to.
    assert(ht_link_next(3, true, false, true, true) == 1 && ht_link_next(1, true, false, true, true) == -1 && ht_link_options(true, false, true, true) == 1);
    assert(ht_link_next(3, false, true, true, true) == 2 && ht_link_next(2, false, true, true, true) == -1);
    assert(ht_link_next(0, false, false, true, true) == -1 && ht_link_options(false, false, true, true) == 0);
    // No phone link in the image: Phone and Auto are never offered, whatever is stored, and the row (two options or more) is not shown.
    assert(ht_link_next(1, true, true, false, true) == -1 && ht_link_next(0, true, true, false, true) == 1 && ht_link_options(true, true, false, true) == 1);
    assert(!ht_link_offered(2, true, true, false, true) && !ht_link_offered(3, true, true, false, true) && ht_link_offered(1, true, true, false, true));
    assert(ht_link_options(true, true, true, true) == 3);
    assert(!strcmp(ht_link_label(0), "USB only") && !strcmp(ht_link_label(1), "Wi-Fi") && !strcmp(ht_link_label(2), "Phone") && !strcmp(ht_link_label(3), "Auto"));
    // Any starting point, any set-up: the cycle returns to where it began and never sticks.
    for (int have = 0; have < 8; have++)
        for (int start = 0; start < 4; start++) {
            const bool w = have & 1, p = have & 2, b = have & 4;
            int seen = start, visited = 0;
            for (int steps = 0; steps < 6; steps++) {
                const int next = ht_link_next(seen, w, p, b, true);
                if (next < 0) { assert(ht_link_options(w, p, b, true) <= 1 && "no way on only when nothing else is on offer"); break; }
                assert(ht_link_offered(next, w, p, b, true) && next != seen);
                if (steps < 3) visited |= 1 << next;
                seen = next;
            }
            if (ht_link_options(w, p, b, true) >= 2) {   // two or more on offer: three taps visit every one of them
                int want = 0;
                for (int mode = 1; mode <= 3; mode++) want |= ht_link_offered(mode, w, p, b, true) ? 1 << mode : 0;
                assert((visited & want) == want);
            }
        }

    // PHONE needs a bond: without one it is never offered, so an unplugged Wi-Fi dial cannot be cycled into an unpaired phone mode. Auto (Wi-Fi first) is.
    assert(!ht_link_offered(2, true, true, true, false) && ht_link_offered(2, true, true, true, true) && ht_link_offered(3, true, true, true, false));
    assert(ht_link_next(1, true, true, true, false) == 3 && ht_link_next(3, true, true, true, false) == 1 && "Wi-Fi and Auto only, no Phone");
    assert(ht_link_next(2, true, true, true, false) == 3 && "a dial already in phone mode with its bond gone can leave it");
    assert(ht_link_options(true, true, true, false) == 2 && ht_link_options(false, true, true, false) == 0 && ht_link_options(true, true, true, true) == 3);
    for (int have = 0; have < 8; have++)   // never PHONE with no bond, whatever else is set up
        for (int start = 0; start < 4; start++)
            assert(ht_link_next(start, have & 1, have & 2, have & 4, false) != 2);

    // ── Night mode: on = 8 percent and muted, the old pair kept; off = the old pair back.
    ht_prefs_t p;
    memset(&p, 0, sizeof p);
    int bright = 70;
    bool muted = false;
    ht_night_toggle(&p, &bright, &muted);
    assert(p.night && bright == HT_NIGHT_BRIGHT && muted && p.night_bright == 70 && !p.night_muted);
    ht_night_toggle(&p, &bright, &muted);
    assert(!p.night && bright == 70 && !muted);
    // The old pair is whatever it was, muted included, and a floor level or a level of 100 comes back as it was.
    bright = 100; muted = true;
    ht_night_toggle(&p, &bright, &muted);
    assert(bright == 8 && muted && p.night_bright == 100 && p.night_muted);
    ht_night_toggle(&p, &bright, &muted);
    assert(bright == 100 && muted);
    bright = 8; muted = false;
    ht_night_toggle(&p, &bright, &muted);
    ht_night_toggle(&p, &bright, &muted);
    assert(bright == 8 && !muted);
    // The user moves the arc while Night mode is on: Night mode is off, the level the user chose stays (the caller keeps it), the sound goes back.
    bright = 55; muted = false;
    ht_night_toggle(&p, &bright, &muted);
    bool m = muted;
    assert(ht_night_user_moved(&p, &m) && !p.night && !m);
    // ... and when it is not on, moving the arc changes nothing here.
    m = true;
    assert(!ht_night_user_moved(&p, &m) && m);

    // ── Battery saver: the panel at 30 percent at most, 30 s to dark, the Wi-Fi radio parked after 2 minutes; off gives the user's own values back.
    memset(&p, 0, sizeof p);
    p.timer = HT_TIMER_5M;
    assert(ht_panel_cap(&p) == 100 && ht_screen_off_ms(&p) == 300000 && ht_park_minutes(&p) == 5);
    p.saver = true;
    assert(ht_panel_cap(&p) == 30 && ht_screen_off_ms(&p) == 30000 && ht_park_minutes(&p) == 2);
    p.saver = false;
    assert(ht_panel_cap(&p) == 100 && ht_screen_off_ms(&p) == 300000 && ht_park_minutes(&p) == 5 && p.timer == HT_TIMER_5M);
    p.timer = HT_TIMER_AUTO;
    assert(ht_screen_off_ms(&p) == 0 && "an untouched timer is the policy's own again");
    p.saver = true; p.night = true;
    assert(ht_panel_cap(&p) == 30 && ht_screen_off_ms(&p) == 30000 && "the two rows do not interfere");

    // ── The radio park after 2 minutes. The park is only a time: pp_wifi_off_wanted starts at the dark span and nothing else, and the mode switch's hold
    // (net_link_radio_waking, mode_glue.c H1) reads the radio's state, not this time (source-checked in test_prefs_source.py).
    pp_off_t o;
    pp_off_init(&o);
    pp_off_in_t in = {.now_ms = 1000, .state = PP_S2, .on_battery = true, .pct = 80};
    assert(pp_off_step(&o, &in) == PP_OFF_NONE);
    assert(!pp_wifi_off_wanted(&o, 1000 + 119999, 120000u) && pp_wifi_off_wanted(&o, 1000 + 120000, 120000u) && "2 min of dark battery time");
    assert(!pp_wifi_off_wanted(&o, 1000 + 120000, 300000u) && "5 min stays the default");
    in.state = PP_S0;
    pp_off_step(&o, &in);
    assert(!pp_wifi_off_wanted(&o, 1000 + 500000, 120000u) && "a cable ends the span, whatever the park time");

    printf("prefs logic: timer cycle, policy with a user time, link cycle (every set-up), Night mode save/restore, Battery saver overrides, 2 min park PASS\n");
    return 0;
}
