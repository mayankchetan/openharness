// The menu prompt's state machine (ui/habitat/menu_prompt.c): open, the 2 s confirm, an early release, the 10 s timeout, a swipe-down dismiss, and the
// rules that keep BOOT from doing anything else while the prompt is up.
#include "../main/ui/habitat/menu_prompt.h"
#include "../main/boot_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Samples BOOT every 100 ms (ptt.c's poll) from `from` to `to` inclusive with the key at `down`; returns the time of the first confirm, or 0.
static uint32_t poll(ht_menu_t *m, uint32_t from, uint32_t to, bool down)
{
    for (uint32_t t = from; t <= to; t += 100)
        if (ht_menu_boot(m, t, down)) return t;
    return 0;
}

// THE REAL PTT LOOP, in miniature (ptt.c): boot_key.c's machine with the UI's two answers. ui_voice_can_start is false while the prompt is up
// (boot_voice refuses), a tap is eaten while the prompt is up or swallowing (ui_boot_pressed), and ui_menu_boot runs after the machine's event.
static ht_menu_t ui;
static int voice_starts, taps_acted, menu_opened;
static bool can_talk(void) { return !ui.open; }
static void ptt_poll(bk_t *k, uint32_t now, bool down)
{
    switch (bk_step(k, now, down, true, can_talk)) {
    case BK_TAP: if (!ht_menu_eats_tap(&ui)) taps_acted++; break;      // ui_boot_pressed: goes home, or stops the turn
    case BK_HOLD_START: voice_starts++; break;                          // ui_voice_start
    default: break;
    }
    if (ht_menu_boot(&ui, now, down)) menu_opened++;                    // ui_menu_boot
}
static void ptt_run(bk_t *k, uint32_t from, uint32_t to, bool down) { for (uint32_t t = from; t <= to; t += 100) ptt_poll(k, t, down); }

int main(void)
{
    ht_menu_t m = {0};

    // BOOT AND THE PROMPT. With the prompt up, a 2 s hold starts NO capture (the 400 ms hold threshold is denied), opens the menu once, and the release is not
    // a tap (it would go home). Without the prompt the same hold starts a capture, as it always did.
    bk_t k;
    bk_init(&k, false);
    memset(&ui, 0, sizeof ui);
    ht_menu_open(&ui, 0, false);
    ptt_run(&k, 100, 1000, false);
    ptt_run(&k, 1100, 3300, true);
    assert(menu_opened == 1 && voice_starts == 0 && "a capture is never started by the prompt's hold");
    ptt_run(&k, 3400, 3600, false);
    assert(taps_acted == 0 && !ui.open && !ht_menu_eats_tap(&ui) && menu_opened == 1 && "the release did not go home");
    // A short press with the prompt up (a tap): nothing, the prompt stays.
    ht_menu_open(&ui, 4000, false);
    ptt_run(&k, 4100, 4200, false);
    ptt_run(&k, 4300, 4500, true);
    ptt_run(&k, 4600, 4800, false);
    assert(taps_acted == 0 && voice_starts == 0 && ui.open && menu_opened == 1);
    // A 1 s hold with the prompt up (early release): no capture, no tap, prompt stays.
    ptt_run(&k, 5000, 5300, false);
    ptt_run(&k, 5400, 6400, true);
    ptt_run(&k, 6500, 6700, false);
    assert(taps_acted == 0 && voice_starts == 0 && ui.open && menu_opened == 1);
    // Prompt gone (dismissed): the same 1 s hold is a capture again, and a tap acts again.
    ht_menu_close(&ui);
    ptt_run(&k, 7000, 7300, false);
    ptt_run(&k, 7400, 8400, true);
    ptt_run(&k, 8500, 8700, false);
    assert(voice_starts == 1 && "the prompt gone, BOOT is voice again");
    ptt_run(&k, 9000, 9100, false);
    ptt_run(&k, 9200, 9300, true);
    ptt_run(&k, 9400, 9600, false);
    assert(taps_acted == 1 && menu_opened == 1);

    // OPEN, then a hold of 2 s confirms once: not before, at the first sample 2000 ms after the press.
    ht_menu_open(&m, 1000, false);
    assert(m.open && ht_menu_eats_tap(&m) && ht_menu_progress(&m, 1000) == 0);
    assert(poll(&m, 1000, 1400, false) == 0);                  // key up: nothing
    assert(!ht_menu_boot(&m, 1500, true) && ht_menu_progress(&m, 1500) == 0);   // the press edge
    assert(ht_menu_progress(&m, 2500) == 50 && ht_menu_progress(&m, 3499) == 99);
    assert(poll(&m, 1600, 3499, true) == 0 && m.open && "1999 ms is not 2 s");
    assert(ht_menu_boot(&m, 3500, true) && "2000 ms is");
    assert(!m.open && ht_menu_eats_tap(&m) && "the release of this press is swallowed");
    assert(!ht_menu_boot(&m, 3600, true) && ht_menu_eats_tap(&m) && "held on: still swallowed, and no second confirm");
    assert(!ht_menu_boot(&m, 3700, false) && !ht_menu_eats_tap(&m) && "released: swallowing ends with that sample");
    assert(!ht_menu_boot(&m, 3800, true) && "a later press with the prompt closed is nothing here");

    // EARLY RELEASE: nothing happens, the prompt stays, the bar is empty again, and a second try works from zero.
    ht_menu_open(&m, 10000, false);
    assert(poll(&m, 10000, 10100, false) == 0);
    assert(poll(&m, 10200, 11800, true) == 0 && ht_menu_progress(&m, 11800) == 80);
    assert(!ht_menu_boot(&m, 11900, false) && m.open && ht_menu_progress(&m, 11900) == 0 && ht_menu_eats_tap(&m));
    assert(poll(&m, 12000, 12100, false) == 0);
    assert(poll(&m, 12200, 14100, true) == 0 && m.open && "the second hold counts from its own press");
    assert(ht_menu_boot(&m, 14200, true));

    // BOOT ALREADY DOWN when the prompt came up does not count; it has to go up and down again.
    ht_menu_open(&m, 20000, true);
    assert(poll(&m, 20000, 25000, true) == 0 && m.open && ht_menu_progress(&m, 25000) == 0);
    assert(poll(&m, 25100, 25200, false) == 0);
    assert(poll(&m, 25300, 27200, true) == 0 && poll(&m, 27300, 27300, true) == 27300);

    // TIMEOUT: 10 s with no action closes it (strictly after), by itself; the tick says so once.
    ht_menu_open(&m, 30000, false);
    assert(!ht_menu_tick(&m, 39999) && m.open && ht_menu_next_ms(&m, 39000) == 1000);
    assert(ht_menu_tick(&m, 40000) && !m.open && !ht_menu_eats_tap(&m));
    assert(!ht_menu_tick(&m, 50000) && ht_menu_next_ms(&m, 50000) == 0);
    // A BOOT press is an action: the 10 s restart from the release, and never run out while the key is held.
    ht_menu_open(&m, 60000, false);
    poll(&m, 60000, 60100, false);
    poll(&m, 66000, 67000, true);
    assert(!ht_menu_tick(&m, 75000) && m.open && "held: no timeout");
    ht_menu_boot(&m, 75100, false);
    assert(!ht_menu_tick(&m, 85000) && ht_menu_tick(&m, 85100) && "10 s from the release");
    // The clock wraps: the same rules over the 32-bit edge.
    ht_menu_open(&m, 0xFFFFF000u, false);
    assert(!ht_menu_tick(&m, 0xFFFFF000u + 9999) && ht_menu_tick(&m, 0xFFFFF000u + 10000));

    // SWIPE DOWN dismisses at once, and a press afterwards does nothing.
    ht_menu_open(&m, 90000, false);
    ht_menu_close(&m);
    assert(!m.open && !ht_menu_eats_tap(&m) && ht_menu_progress(&m, 90100) == 0);
    assert(poll(&m, 90100, 93000, true) == 0 && !m.open);
    // Dismissed while BOOT is held, then released: nothing is swallowed (no confirm happened).
    ht_menu_open(&m, 100000, false);
    poll(&m, 100000, 100100, false);
    poll(&m, 100200, 100800, true);
    ht_menu_close(&m);
    assert(!m.down && !ht_menu_eats_tap(&m));

    // A BOOT TAP while the prompt is up is eaten (ui_boot_pressed asks): the prompt stays up and nothing else runs.
    ht_menu_open(&m, 110000, false);
    poll(&m, 110000, 110100, false);
    poll(&m, 110200, 110500, true);
    ht_menu_boot(&m, 110600, false);
    assert(m.open && ht_menu_eats_tap(&m));

    // Opening again restarts everything.
    ht_menu_open(&m, 120000, false);
    assert(m.open && !m.down && !m.swallow && m.since_ms == 120000);

    printf("menu prompt: open, 2 s confirm (once), early release, BOOT already down, 10 s timeout (held, wrap), swipe-down dismiss, taps eaten, BOOT starts no voice while the prompt is up PASS\n");
    return 0;
}
