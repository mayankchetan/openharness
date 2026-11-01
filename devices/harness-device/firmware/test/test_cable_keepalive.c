// Host test for the keepalive timing rule (cable_keepalive.h; task W13). The table is vectors/keepalive.txt, which the daemon's spec
// reads too, so the two halves cannot drift apart without one of the suites failing.
//
//   cc -std=c11 -Wall -Wextra -Werror -I ../main -o test_cable_keepalive test_cable_keepalive.c && ./test_cable_keepalive vectors/keepalive.txt
#include "cable_keepalive.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(int argc, char **argv)
{
    // Today's timings, exactly (the regression): the default is the old 5 s ping, so the dial's silence window is the old 15 s and the
    // daemon's the old 20 s. The daemon's window is later than the dial's.
    CHECK(KEEPALIVE_DEFAULT_MS == 5000u);
    CHECK(keepalive_dial_silence_ms(KEEPALIVE_DEFAULT_MS) == 15000u);
    CHECK(keepalive_daemon_silence_ms(KEEPALIVE_DEFAULT_MS) == 20000u);
    CHECK(KEEPALIVE_MIN_MS == 5000u && KEEPALIVE_MAX_MS == 60000u);
    CHECK(keepalive_effective_ms(NAN) == KEEPALIVE_DEFAULT_MS);

    // Every K in range: 1:3 scaling, the daemon later than the dial by the same margin, and a ping always lands inside the dial's window.
    for (uint32_t k = KEEPALIVE_MIN_MS; k <= KEEPALIVE_MAX_MS; k += 250) {
        CHECK(keepalive_effective_ms((double)k) == k);
        CHECK(keepalive_dial_silence_ms(k) == 3u * k);
        CHECK(keepalive_daemon_silence_ms(k) > keepalive_dial_silence_ms(k));
        CHECK(keepalive_daemon_silence_ms(k) - keepalive_dial_silence_ms(k) == 5000u);
        CHECK(keepalive_dial_silence_ms(k) >= 3u * k);   // two pings may be lost before the dial gives up
    }
    // Clamp and monotone around the edges.
    CHECK(keepalive_effective_ms(4999.0) == KEEPALIVE_MIN_MS);
    CHECK(keepalive_effective_ms(60001.0) == KEEPALIVE_MAX_MS);
    CHECK(keepalive_effective_ms(INFINITY) == KEEPALIVE_MAX_MS);
    CHECK(keepalive_effective_ms(-INFINITY) == KEEPALIVE_MIN_MS);

    // The shared table.
    int rows = 0;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "case ", 5) != 0) continue;
            char name[64], raw[64];
            unsigned long k, dial, daemon;
            if (sscanf(line, "case %63s %63s %lu %lu %lu", name, raw, &k, &dial, &daemon) != 5) { printf("bad row: %s", line); g_fail++; continue; }
            const double v = !strcmp(raw, "absent") || !strcmp(raw, "nan") ? NAN : strtod(raw, NULL);
            const uint32_t got = keepalive_effective_ms(v);
            g_checks += 3;
            if (got != k || keepalive_dial_silence_ms(got) != dial || keepalive_daemon_silence_ms(got) != daemon) {
                g_fail++;
                printf("FAIL row %s: K %u dial %u daemon %u, want %lu %lu %lu\n", name, (unsigned)got, (unsigned)keepalive_dial_silence_ms(got),
                       (unsigned)keepalive_daemon_silence_ms(got), k, dial, daemon);
            }
            rows++;
        }
        fclose(f);
    }
    CHECK(argc > 1 && rows >= 12);
    printf("cable_keepalive: %d checks, %d table rows, %d failed\n", g_checks, rows, g_fail);
    return g_fail ? 1 : 0;
}
