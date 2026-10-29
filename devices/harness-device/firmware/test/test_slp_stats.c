// slp_stats.c: the percentile and the 50-cycle summary line of the sleep/wake profile (DEVICE_SLEEP_PROFILE).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "slp_stats.h"

int main(void)
{
    // Nearest rank on a sorted copy; the input is left alone.
    uint32_t v[10] = {50, 10, 40, 30, 20, 100, 90, 80, 70, 60};
    assert(slp_percentile(v, 10, 50) == 50);     // rank ceil(0.5 * 10) = 5th smallest
    assert(slp_percentile(v, 10, 99) == 100);    // rank 10
    assert(slp_percentile(v, 10, 100) == 100);
    assert(slp_percentile(v, 10, 10) == 10);     // rank 1
    assert(slp_percentile(v, 10, 0) == 10);
    assert(v[0] == 50 && v[5] == 100);           // not sorted in place
    assert(slp_percentile(v, 1, 99) == 50);
    assert(slp_percentile(v, 0, 50) == 0);
    uint32_t hundred[100];
    for (int i = 0; i < 100; i++) hundred[99 - i] = (uint32_t)(i + 1);
    assert(slp_percentile(hundred, 100, 50) == 50 && slp_percentile(hundred, 100, 99) == 99);

    // The summary line: cycles, wake and sleep percentiles and max, the free-memory drift (signed), the largest block.
    uint32_t wake[5] = {300, 280, 900, 310, 290}, sleep[5] = {40, 41, 39, 120, 40};
    char line[300];
    int n = slp_format_summary(line, sizeof line, 1, 5, wake, sleep, 5, 150000, 152000, 65536, 2);
    assert(n > 0 && (size_t)n == strlen(line));
    assert(!strcmp(line, "SLP summary cycles=1-5 wake_ms p50=300 p99=900 max=900 sleep_ms p50=40 p99=120 max=120 "
                         "int_free=150000 drift=-2000 largest=65536 alloc_fail=2"));
    n = slp_format_summary(line, sizeof line, 51, 100, wake, sleep, 5, 153000, 152000, 65536, 0);
    assert(strstr(line, "cycles=51-100") && strstr(line, "drift=1000"));
    // The longest line the formatter can make fits the 512 B log limit with room for the log prefix.
    uint32_t big[50];
    for (int i = 0; i < 50; i++) big[i] = 4000000000u;
    n = slp_format_summary(line, sizeof line, 2147483647, 2147483647, big, big, 50, 2000000000, -2000000000, 2000000000, 2000000000);
    assert(n > 0 && n < 300);
    puts("sleep profile stats: nearest-rank percentiles, summary line with signed drift PASS");
    return 0;
}
