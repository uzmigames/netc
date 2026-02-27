/**
 * bench_stats.h — Percentile statistics for benchmark timing data.
 *
 * Computes p50, p90, p99, p999, mean, and population stddev from an array
 * of uint64_t nanosecond timing samples per RFC-002 §5.4.
 *
 * p99 = 99,000th smallest value (0-indexed, sorted array, 100,000 samples).
 *
 * Usage:
 *   uint64_t samples[N];
 *   // ... fill samples ...
 *   bench_stats_t st;
 *   bench_stats_compute(&st, samples, N);
 *   printf("p99 = %" PRIu64 " ns\n", st.p99_ns);
 */

#ifndef BENCH_STATS_H
#define BENCH_STATS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Result structure
 * ========================================================================= */
typedef struct {
    uint64_t p50_ns;      /* 50th percentile (median)   */
    uint64_t p90_ns;      /* 90th percentile            */
    uint64_t p99_ns;      /* 99th percentile            */
    uint64_t p999_ns;     /* 99.9th percentile          */
    uint64_t min_ns;      /* Minimum observed           */
    uint64_t max_ns;      /* Maximum observed           */
    double   mean_ns;     /* Arithmetic mean            */
    double   stddev_ns;   /* Population standard deviation */
    size_t   count;       /* Number of samples          */
} bench_stats_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Compute statistics from 'n' timing samples (in nanoseconds).
 * The samples array is sorted in-place for percentile calculation.
 * 'n' must be >= 1.
 */
void bench_stats_compute(bench_stats_t *st, uint64_t *samples, size_t n);

/**
 * Derived throughput helpers.
 *
 * bench_stats_throughput_mbs: given bytes-per-iteration and mean latency,
 *   returns sustained throughput in MB/s.
 *
 * bench_stats_mpps: given mean latency (ns), returns millions-of-packets/s.
 */
double bench_stats_throughput_mbs(size_t bytes_per_pkt, double mean_ns);
double bench_stats_mpps(double mean_ns);

/**
 * Compression ratio helper.
 *   ratio = compressed_total_bytes / original_total_bytes
 */
double bench_stats_ratio(uint64_t original_bytes, uint64_t compressed_bytes);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_STATS_H */
