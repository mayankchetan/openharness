// slp_stats.c: the percentile and the 50-cycle summary line of the sleep/wake profile (DEVICE_SLEEP_PROFILE).
// Times are signed microseconds (a stamp that precedes its arm point is a negative number, never an unsigned wrap).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "slp_stats.h"

int main(void)
{
    // Nearest rank on a sorted copy; the input is left alone.
    int32_t v[10] = {50, 10, 40, 30, 20, 100, 90, 80, 70, 60};
    assert(slp_percentile(v, 10, 50) == 50);     // rank ceil(0.5 * 10) = 5th smallest
    assert(slp_percentile(v, 10, 99) == 100);    // rank 10
    assert(slp_percentile(v, 10, 100) == 100);
    assert(slp_percentile(v, 10, 10) == 10);     // rank 1
    assert(slp_percentile(v, 10, 0) == 10);
    assert(v[0] == 50 && v[5] == 100);           // not sorted in place
    assert(slp_percentile(v, 1, 99) == 50);
    assert(slp_percentile(v, 0, 50) == 0);
    int32_t hundred[100];
    for (int i = 0; i < 100; i++) hundred[99 - i] = i + 1;
    assert(slp_percentile(hundred, 100, 50) == 50 && slp_percentile(hundred, 100, 99) == 99);
    int32_t signed_v[3] = {-193000, 28000, 28431};   // a negative delta sorts first and is not a huge unsigned number
    assert(slp_percentile(signed_v, 3, 0) == -193000 && slp_percentile(signed_v, 3, 100) == 28431);

    // A microsecond value printed as milliseconds with three decimals, sign kept.
    char text[16];
    assert(slp_format_ms(text, sizeof text, 28431) == 6 && !strcmp(text, "28.431"));
    slp_format_ms(text, sizeof text, 0); assert(!strcmp(text, "0.000"));
    slp_format_ms(text, sizeof text, 150); assert(!strcmp(text, "0.150"));
    slp_format_ms(text, sizeof text, -193040); assert(!strcmp(text, "-193.040"));
    slp_format_ms(text, sizeof text, -5); assert(!strcmp(text, "-0.005"));

    // The summary line: cycles, samples and excluded cycles, wake and sleep percentiles and max in ms (3 decimals), the
    // free-memory drift (signed), the largest block.
    int32_t wake[5] = {28400, 28100, 90000, 31000, 28300}, sleep[5] = {140, 150, 120, 2000, 160};
    char line[400];
    int n = slp_format_summary(line, sizeof line, 1, 5, wake, sleep, 5, 0, 150000, 152000, 65536, 2);
    assert(n > 0 && (size_t)n == strlen(line));
    assert(!strcmp(line, "SLP summary cycles=1-5 samples=5 excluded=0 wake_ms p50=28.400 p99=90.000 max=90.000 "
                         "sleep_ms p50=0.150 p99=2.000 max=2.000 int_free=150000 drift=-2000 largest=65536 alloc_fail=2"));
    n = slp_format_summary(line, sizeof line, 51, 100, wake, sleep, 4, 1, 153000, 152000, 65536, 0);
    assert(strstr(line, "cycles=51-100 samples=4 excluded=1") && strstr(line, "drift=1000"));
    // A negative delta never prints as a wrapped number.
    int32_t odd[2] = {-193040, 28000};
    n = slp_format_summary(line, sizeof line, 1, 2, odd, odd, 2, 0, 0, 0, 0, 0);
    assert(strstr(line, "wake_ms p50=-193.040") && !strstr(line, "4294"));
    // The longest line the formatter can make fits the 512 B log limit with room for the log prefix.
    int32_t big[50];
    for (int i = 0; i < 50; i++) big[i] = -2000000000;
    n = slp_format_summary(line, sizeof line, 2147483647, 2147483647, big, big, 50, 50, 2000000000, -2000000000, 2000000000, 2000000000);
    assert(n > 0 && n < 360);
    puts("sleep profile stats: signed microsecond percentiles, ms with 3 decimals, summary line with signed drift PASS");
    return 0;
}
