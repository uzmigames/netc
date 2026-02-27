/**
 * bench_stats.c — Percentile statistics for benchmark timing data.
 */

#include "bench_stats.h"
#include <stdlib.h>
#include <math.h>

/* =========================================================================
 * Comparison function for qsort
 * ========================================================================= */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) return -1;
    if (x > y) return  1;
    return 0;
}

/* =========================================================================
 * bench_stats_compute
 * ========================================================================= */
void bench_stats_compute(bench_stats_t *st, uint64_t *samples, size_t n)
{
    if (!st || !samples || n == 0) return;

    /* Sort ascending */
    qsort(samples, n, sizeof(uint64_t), cmp_u64);

    /* Percentiles: index = floor(p/100 * n), clamped to [0, n-1] */
    #define PCT_IDX(p) ((size_t)((p) / 100.0 * (double)(n)) < (n) \
                        ? (size_t)((p) / 100.0 * (double)(n))      \
                        : (n) - 1u)

    st->p50_ns  = samples[PCT_IDX(50.0)];
    st->p90_ns  = samples[PCT_IDX(90.0)];
    st->p99_ns  = samples[PCT_IDX(99.0)];
    st->p999_ns = samples[PCT_IDX(99.9)];
    st->min_ns  = samples[0];
    st->max_ns  = samples[n - 1];
    st->count   = n;

    #undef PCT_IDX

    /* Mean and population stddev (two-pass for numerical stability) */
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)samples[i];
    }
    double mean = sum / (double)n;
    st->mean_ns = mean;

    double var_sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double diff = (double)samples[i] - mean;
        var_sum += diff * diff;
    }
    st->stddev_ns = sqrt(var_sum / (double)n);
}

/* =========================================================================
 * Derived metrics
 * ========================================================================= */

double bench_stats_throughput_mbs(size_t bytes_per_pkt, double mean_ns)
{
    if (mean_ns <= 0.0) return 0.0;
    /* bytes/ns = bytes * 1e9 / ns / 1e6 = bytes * 1000 / ns   (→ MB/s) */
    return (double)bytes_per_pkt * 1000.0 / mean_ns;
}

double bench_stats_mpps(double mean_ns)
{
    if (mean_ns <= 0.0) return 0.0;
    /* 1 packet / mean_ns = 1e9/mean_ns packets/s / 1e6 = 1000/mean_ns Mpps */
    return 1000.0 / mean_ns;
}

double bench_stats_ratio(uint64_t original_bytes, uint64_t compressed_bytes)
{
    if (original_bytes == 0) return 0.0;
    return (double)compressed_bytes / (double)original_bytes;
}
