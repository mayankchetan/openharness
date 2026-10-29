#pragma once
// The statistics of the sleep/wake profile (DEVICE_SLEEP_PROFILE, sleep_profile.c), kept free of the SDK so a host test
// can check them. Times are SIGNED microseconds: a stamp that precedes its arm point is a negative number the caller
// can see and exclude, never an unsigned wrap (the first run printed 4294967103 ms).
#include <stddef.h>
#include <stdint.h>

// Nearest-rank percentile (pct 0..100) of v[0..n): the ceil(pct/100 * n)-th smallest, rank at least 1. 0 for n == 0.
// Works on a copy; v is not reordered. n is at most 100.
int32_t slp_percentile(const int32_t *v, int n, int pct);

// A microsecond value as milliseconds with three decimals ("28.431", "-0.005"). Returns the length written.
int slp_format_ms(char *out, size_t size, int32_t us);

// "SLP summary cycles=<first>-<last> samples=<n> excluded=<k> wake_ms p50= p99= max= sleep_ms p50= p99= max= int_free= drift=
// largest= alloc_fail=" over the n samples of one window (k = cycles left out because someone else woke the panel);
// times in ms with 3 decimals; drift = int_free - int_free_first (negative = memory lost since the first cycle).
int slp_format_summary(char *out, size_t size, int first_cycle, int last_cycle, const int32_t *wake_us,
                       const int32_t *sleep_us, int n, int excluded, int int_free, int int_free_first, int largest,
                       int alloc_fail);
