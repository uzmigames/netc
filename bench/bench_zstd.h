/**
 * bench_zstd.h — Zstd compressor adapter.
 *
 * Compiled only when NETC_BENCH_WITH_ZSTD is defined (Zstd detected by CMake).
 * Provides three configurations:
 *   bench_zstd_create(1,  NULL) — Zstd level 1 (fastest)
 *   bench_zstd_create(3,  NULL) — Zstd level 3 (default)
 *   bench_zstd_create(1,  corpus_pkts, lens, n) — Zstd level 1 with trained dict
 *
 * Returns NULL if Zstd is not compiled in or on allocation failure.
 * Caller must call c->destroy(c) when done.
 */

#ifndef BENCH_ZSTD_H
#define BENCH_ZSTD_H

#include "bench_compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a Zstd adapter.
 * level: compression level (1..22; 1=fastest, 3=default)
 * use_dict: if non-zero, train a Zstd dictionary on the workload during train()
 */
bench_compressor_t *bench_zstd_create(int level, int use_dict);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_ZSTD_H */
