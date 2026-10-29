#include "slp_stats.h"

#include <stdio.h>
#include <string.h>

int32_t slp_percentile(const int32_t *v, int n, int pct)
{
    if (n <= 0) return 0;
    if (n > 100) n = 100;
    int32_t sorted[100];
    memcpy(sorted, v, (size_t)n * sizeof sorted[0]);
    for (int i = 1; i < n; i++) {   // insertion sort: n is tiny
        int32_t x = sorted[i];
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

int slp_format_ms(char *out, size_t size, int32_t us)
{
    int64_t a = us < 0 ? -(int64_t)us : us;
    return snprintf(out, size, "%s%lld.%03lld", us < 0 ? "-" : "", (long long)(a / 1000), (long long)(a % 1000));
}

int slp_format_summary(char *out, size_t size, int first_cycle, int last_cycle, const int32_t *wake_us,
                       const int32_t *sleep_us, int n, int excluded, int int_free, int int_free_first, int largest,
                       int alloc_fail)
{
    char w50[16], w99[16], wmax[16], s50[16], s99[16], smax[16];
    slp_format_ms(w50, sizeof w50, slp_percentile(wake_us, n, 50));
    slp_format_ms(w99, sizeof w99, slp_percentile(wake_us, n, 99));
    slp_format_ms(wmax, sizeof wmax, slp_percentile(wake_us, n, 100));
    slp_format_ms(s50, sizeof s50, slp_percentile(sleep_us, n, 50));
    slp_format_ms(s99, sizeof s99, slp_percentile(sleep_us, n, 99));
    slp_format_ms(smax, sizeof smax, slp_percentile(sleep_us, n, 100));
    return snprintf(out, size,
        "SLP summary cycles=%d-%d samples=%d excluded=%d wake_ms p50=%s p99=%s max=%s sleep_ms p50=%s p99=%s max=%s "
        "int_free=%d drift=%d largest=%d alloc_fail=%d",
        first_cycle, last_cycle, n, excluded, w50, w99, wmax, s50, s99, smax, int_free, int_free - int_free_first, largest,
        alloc_fail);
}
