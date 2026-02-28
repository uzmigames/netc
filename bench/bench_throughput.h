/**
 * bench_throughput.h — Sustained throughput and Mpps benchmarks.
 *
 * RFC-002 §5 defines two additional benchmark modes beyond per-packet latency:
 *
 *   Throughput (4.2):
 *     Compress/decompress a full corpus of N packets in one continuous loop.
 *     Report total_bytes_compressed / elapsed_seconds as MB/s.
 *     Unlike the latency benchmark, individual packets are NOT timed separately.
 *
 *   Mpps (4.3):
 *     Compress 1,000,000 packets and report the wall-clock rate as Mpps
 *     (millions of packets per second).
 *
 * Both modes work with any bench_compressor_t adapter.
 */

#ifndef BENCH_THROUGHPUT_H
#define BENCH_THROUGHPUT_H

#include "bench_compressor.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Throughput result
 * ========================================================================= */

typedef struct {
    const char      *compressor;
    const char      *compressor_cfg;
    bench_workload_t workload;

    uint64_t  packets;            /* number of packets processed */
    uint64_t  original_bytes;     /* total uncompressed bytes */
    uint64_t  compressed_bytes;   /* total compressed bytes */
    double    ratio;              /* compressed / original */

    double    compress_mbs;       /* compression throughput MB/s   */
    double    decompress_mbs;     /* decompression throughput MB/s */
    double    compress_mpps;      /* compression rate Mpps         */
    double    decompress_mpps;    /* decompression rate Mpps       */

    double    compress_elapsed_s;   /* wall time for compression run   */
    double    decompress_elapsed_s; /* wall time for decompression run */
} bench_throughput_result_t;

/* =========================================================================
 * Configuration
 * ========================================================================= */

typedef struct {
    size_t   warmup;        /* warmup packet count (not timed) */
    size_t   count;         /* measurement packet count        */
    uint64_t seed;
} bench_throughput_cfg_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Run a sustained throughput benchmark.
 *
 * Generates cfg->warmup packets (untimed warm-up), then cfg->count packets
 * timed as a single bulk run. Reports MB/s and Mpps.
 *
 * Returns 0 on success, -1 on error.
 */
int bench_throughput_run(const bench_throughput_cfg_t *cfg,
                         bench_workload_t               wl,
                         bench_compressor_t            *c,
                         bench_throughput_result_t     *out);

/**
 * Run an Mpps benchmark (1,000,000-packet fixed run).
 *
 * Equivalent to bench_throughput_run with cfg->count = 1,000,000.
 * Returns Mpps in out->compress_mpps and out->decompress_mpps.
 */
int bench_mpps_run(bench_workload_t           wl,
                   bench_compressor_t         *c,
                   uint64_t                    seed,
                   bench_throughput_result_t  *out);

/** Print a throughput result to stdout (table format). */
void bench_throughput_print(const bench_throughput_result_t *r);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_THROUGHPUT_H */
