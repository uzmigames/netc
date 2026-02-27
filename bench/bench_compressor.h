/**
 * bench_compressor.h — Generic compressor interface for the benchmark harness.
 *
 * All compressor adapters (zlib, LZ4, Zstd, netc) implement this interface
 * so that bench_main.c can drive any compressor uniformly.
 *
 * A bench_compressor_t is a vtable + opaque state pointer.
 * Adapters are initialized per-workload, run for warmup+count iterations,
 * then destroyed.
 */

#ifndef BENCH_COMPRESSOR_H
#define BENCH_COMPRESSOR_H

#include "bench_corpus.h"
#include "bench_reporter.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Generic compressor vtable
 * ========================================================================= */

/**
 * All functions return 0 on success, non-zero on error.
 * compress/decompress return the output byte count, 0 on error.
 */
typedef struct bench_compressor {
    const char *name;       /* "zlib-1", "lz4-fast", "netc-dict", etc. */
    const char *cfg;        /* human-readable config string */

    void *state;            /* adapter private state */

    /** Optional: called once before the benchmark loop begins (after init). */
    int  (*train)(struct bench_compressor *c, bench_workload_t wl,
                  uint64_t seed, size_t n);

    /** Called before each benchmark run to reset per-connection state. */
    void (*reset)(struct bench_compressor *c);

    /** Compress src[0..src_len) into dst[0..dst_cap). Returns output len or 0. */
    size_t (*compress)(struct bench_compressor *c,
                       const uint8_t *src, size_t src_len,
                       uint8_t       *dst, size_t dst_cap);

    /** Decompress src[0..src_len) into dst[0..dst_cap). Returns output len or 0. */
    size_t (*decompress)(struct bench_compressor *c,
                         const uint8_t *src, size_t src_len,
                         uint8_t       *dst, size_t dst_cap);

    /** Free all resources. */
    void (*destroy)(struct bench_compressor *c);
} bench_compressor_t;

/* =========================================================================
 * bench_run_generic — run a timing loop for any compressor
 * ========================================================================= */

typedef struct {
    size_t   warmup;
    size_t   count;
    uint64_t seed;
} bench_generic_cfg_t;

/**
 * Run warmup + count timing iterations using the given compressor and workload.
 * Fills *out with timing stats and ratio.
 * Returns 0 on success (all roundtrips matched), -1 on error or mismatch.
 */
int bench_run_generic(const bench_generic_cfg_t *cfg,
                      bench_workload_t            wl,
                      bench_compressor_t         *c,
                      bench_result_t             *out);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_COMPRESSOR_H */
