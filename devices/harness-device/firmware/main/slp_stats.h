#pragma once
// The statistics of the sleep/wake profile (DEVICE_SLEEP_PROFILE, sleep_profile.c), kept free of the SDK so a host test
// can check them.
#include <stddef.h>
#include <stdint.h>

// Nearest-rank percentile (pct 0..100) of v[0..n): the ceil(pct/100 * n)-th smallest, rank at least 1. 0 for n == 0.
// Works on a copy; v is not reordered. n is at most 100.
uint32_t slp_percentile(const uint32_t *v, int n, int pct);

// "SLP summary cycles=<first>-<last> wake_ms p50= p99= max= sleep_ms p50= p99= max= int_free= drift= largest= alloc_fail="
// over the n samples of one window; drift = int_free - int_free_first (negative = memory lost since the first cycle).
int slp_format_summary(char *out, size_t size, int first_cycle, int last_cycle, const uint32_t *wake_ms,
                       const uint32_t *sleep_ms, int n, int int_free, int int_free_first, int largest, int alloc_fail);
