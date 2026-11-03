// S2 WAKE DIAGNOSIS (2026-11-02), built only with -DDEVICE_S2_DIAG=1 (main/CMakeLists.txt) and the overlay sdkconfig.defaults.diag. Not for a product image.
// The pure part: count the wake causes of every light sleep and print them as one line. power_sleep.c feeds it from the light-sleep exit callback.
// Cause numbers are esp_sleep_wakeup_cause_t of ESP-IDF 5.5.6 (esp_sleep.h); esp_sleep_get_wakeup_causes() is a bitmap of 1 << cause.
// A TIMER wake is any FreeRTOS tick timeout or esp_timer alarm; WIFI is the radio (beacons, frames); GPIO is a pin. test/test_s2_diag.py checks the numbers.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define S2D_CAUSES 18

static inline const char *s2d_cause_name(unsigned i)
{
    static const char *const names[S2D_CAUSES] = {
        "NONE",  /* 0 */ "ALL",   /* 1 */ "EXT0",  /* 2 */ "EXT1",  /* 3 */ "TIMER",  /* 4 */ "TOUCHPAD",  /* 5 */ "ULP", /* 6 */ "GPIO", /* 7 */ "UART", /* 8 */
        "UART1", /* 9 */ "UART2", /* 10 */ "WIFI", /* 11 */ "COCPU", /* 12 */ "COCPU_TRAP", /* 13 */ "BT", /* 14 */ "VAD", /* 15 */ "VBAT", /* 16 */ "USB", /* 17 */
    };
    return i < S2D_CAUSES ? names[i] : "?";
}

// One light sleep ended with `causes`: every cause in the bitmap is counted once; an empty bitmap counts as NONE; bits beyond the table are ignored.
static inline void s2d_note(uint32_t counts[S2D_CAUSES], uint32_t causes)
{
    if (!causes) { counts[0]++; return; }
    for (unsigned i = 0; i < S2D_CAUSES; i++)
        if (causes & (1u << i)) counts[i]++;
}

// "wake causes in 60.0 s: TIMER=912 (15.20/s) WIFI=588 (9.80/s)": the difference of two snapshots, zero counts left out, "none" when nothing counted.
static inline int s2d_format(char *out, size_t cap, const uint32_t now[S2D_CAUSES], const uint32_t before[S2D_CAUSES], uint32_t window_ms)
{
    if (!cap) return 0;
    int n = snprintf(out, cap, "wake causes in %u.%u s:", (unsigned)(window_ms / 1000), (unsigned)(window_ms % 1000 / 100));
    if (n < 0) { out[0] = 0; return 0; }
    if ((size_t)n >= cap) return (int)cap - 1;
    int any = 0;
    for (unsigned i = 0; i < S2D_CAUSES; i++) {
        const uint32_t d = now[i] - before[i];
        if (!d) continue;
        any = 1;
        const unsigned hundredths = window_ms ? (unsigned)((uint64_t)d * 100000u / window_ms) : 0;
        const int w = snprintf(out + n, cap - (size_t)n, " %s=%u (%u.%02u/s)", s2d_cause_name(i), (unsigned)d, hundredths / 100, hundredths % 100);
        if (w < 0) break;
        if ((size_t)w >= cap - (size_t)n) return (int)cap - 1;
        n += w;
    }
    if (!any) {
        const int w = snprintf(out + n, cap - (size_t)n, " none");
        if (w >= 0 && (size_t)w < cap - (size_t)n) n += w;
        else return (int)cap - 1;
    }
    return n;
}
