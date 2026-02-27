/**
 * bench_reporter.h — Table, CSV, and JSON output for benchmark results.
 *
 * Three output formats per RFC-002 §7.3 and §7.4:
 *   BENCH_FMT_TABLE — human-readable aligned table (default)
 *   BENCH_FMT_CSV   — comma-separated per RFC-002 §7.3 schema
 *   BENCH_FMT_JSON  — JSON per RFC-002 §7.4 schema
 *
 * Usage:
 *   bench_reporter_t *r = bench_reporter_open(BENCH_FMT_TABLE, stdout);
 *   bench_reporter_begin(r);                     // write header
 *   bench_reporter_write(r, &result);            // write one row
 *   bench_reporter_write(r, &result2);
 *   bench_reporter_end(r);                       // write footer / closing brackets
 *   bench_reporter_close(r);
 */

#ifndef BENCH_REPORTER_H
#define BENCH_REPORTER_H

#include "bench_stats.h"
#include "bench_corpus.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Output format
 * ========================================================================= */
typedef enum {
    BENCH_FMT_TABLE = 0,
    BENCH_FMT_CSV   = 1,
    BENCH_FMT_JSON  = 2,
} bench_format_t;

/* =========================================================================
 * A single benchmark result row
 * ========================================================================= */
typedef struct {
    const char      *compressor;     /* "netc", "lz4", "zstd", etc. */
    const char      *compressor_cfg; /* "delta+simd", "level=1", etc. */
    bench_workload_t workload;
    size_t           pkt_size;       /* bytes per packet (0 = variable) */
    uint64_t         count;          /* number of iterations measured */
    uint64_t         seed;

    bench_stats_t    compress;       /* timing distribution for compression */
    bench_stats_t    decompress;     /* timing distribution for decompression */

    double           ratio;          /* compressed / original */
    uint64_t         original_bytes;
    uint64_t         compressed_bytes;

    /* Derived (filled in by bench_reporter_write if zero) */
    double           compress_mbs;
    double           compress_mpps;
    double           decompress_mbs;
    double           decompress_mpps;
} bench_result_t;

/* =========================================================================
 * Reporter handle
 * ========================================================================= */
typedef struct bench_reporter bench_reporter_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/** Open a reporter writing to 'fp' in the given format.
 *  If 'fp' is NULL, stdout is used.
 *  Returns NULL on allocation failure. */
bench_reporter_t *bench_reporter_open(bench_format_t fmt, FILE *fp);

/** Write format-specific header / JSON opening brace. */
void bench_reporter_begin(bench_reporter_t *r,
                          const char *version,
                          const char *cpu_desc);

/** Write one result row. Derived fields (mbs/mpps) computed if zero. */
void bench_reporter_write(bench_reporter_t *r, bench_result_t *res);

/** Write format-specific footer / JSON closing bracket. */
void bench_reporter_end(bench_reporter_t *r);

/** Free the reporter (does NOT fclose fp). */
void bench_reporter_close(bench_reporter_t *r);

/** Parse format string ("table"/"csv"/"json") → enum. */
bench_format_t bench_format_parse(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_REPORTER_H */
