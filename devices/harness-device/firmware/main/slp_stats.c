#include "slp_stats.h"

#include <stdio.h>
#include <string.h>

uint32_t slp_percentile(const uint32_t *v, int n, int pct)
{
    if (n <= 0) return 0;
    if (n > 100) n = 100;
    uint32_t sorted[100];
    memcpy(sorted, v, (size_t)n * sizeof sorted[0]);
    for (int i = 1; i < n; i++) {   // insertion sort: n is tiny
        uint32_t x = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > x) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = x;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int rank = (pct * n + 99) / 100;   // ceil(pct * n / 100)
    if (rank < 1) rank = 1;
    return sorted[rank - 1];
}

int slp_format_summary(char *out, size_t size, int first_cycle, int last_cycle, const uint32_t *wake_ms,
                       const uint32_t *sleep_ms, int n, int int_free, int int_free_first, int largest, int alloc_fail)
{
    return snprintf(out, size,
        "SLP summary cycles=%d-%d wake_ms p50=%lu p99=%lu max=%lu sleep_ms p50=%lu p99=%lu max=%lu "
        "int_free=%d drift=%d largest=%d alloc_fail=%d",
        first_cycle, last_cycle,
        (unsigned long)slp_percentile(wake_ms, n, 50), (unsigned long)slp_percentile(wake_ms, n, 99),
        (unsigned long)slp_percentile(wake_ms, n, 100),
        (unsigned long)slp_percentile(sleep_ms, n, 50), (unsigned long)slp_percentile(sleep_ms, n, 99),
        (unsigned long)slp_percentile(sleep_ms, n, 100),
        int_free, int_free - int_free_first, largest, alloc_fail);
}
