#include "menu_prompt.h"

void ht_menu_open(ht_menu_t *m, uint32_t now_ms, bool boot_down_now)
{
    m->open = true;
    m->armed = !boot_down_now;
    m->down = false;
    m->swallow = false;
    m->since_ms = now_ms;
    m->down_ms = 0;
}

void ht_menu_close(ht_menu_t *m)
{
    m->open = false;
    m->down = false;
    m->armed = false;
}

bool ht_menu_boot(ht_menu_t *m, uint32_t now_ms, bool down)
{
    if (m->swallow && !down) m->swallow = false;   // the confirming press is over: its release has been eaten by now
    if (!m->open) return false;
    if (!down) {
        m->armed = true;
        if (m->down) m->since_ms = now_ms;          // an early release is an action: the 10 s start again
        m->down = false;
        return false;
    }
    if (!m->armed) return false;                    // BOOT was already down when the prompt came up
    if (!m->down) {
        m->down = true;
        m->down_ms = now_ms;
        m->since_ms = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - m->down_ms) >= HT_MENU_HOLD_MS) {
        ht_menu_close(m);
        m->swallow = true;
        return true;
    }
    return false;
}

bool ht_menu_tick(ht_menu_t *m, uint32_t now_ms)
{
    if (!m->open || m->down) return false;
    if ((uint32_t)(now_ms - m->since_ms) < HT_MENU_TIMEOUT_MS) return false;
    ht_menu_close(m);
    return true;
}

int ht_menu_progress(const ht_menu_t *m, uint32_t now_ms)
{
    if (!m->open || !m->down) return 0;
    const uint32_t held = (uint32_t)(now_ms - m->down_ms);
    return held >= HT_MENU_HOLD_MS ? 100 : (int)(held * 100u / HT_MENU_HOLD_MS);
}

bool ht_menu_eats_tap(const ht_menu_t *m) { return m->open || m->swallow; }

uint32_t ht_menu_next_ms(const ht_menu_t *m, uint32_t now_ms)
{
    if (!m->open) return 0;
    if (m->down) return 100;
    const uint32_t left = HT_MENU_TIMEOUT_MS - (uint32_t)(now_ms - m->since_ms);
    return left > HT_MENU_TIMEOUT_MS ? 1 : left ? left : 1;
}
