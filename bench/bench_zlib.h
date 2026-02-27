/**
 * bench_zlib.h — zlib compressor adapter.
 *
 * Compiled only when NETC_BENCH_WITH_ZLIB is defined (zlib detected by CMake).
 * Provides two bench_compressor_t instances:
 *   bench_zlib_create(1)  — zlib level 1 (fastest)
 *   bench_zlib_create(6)  — zlib level 6 (default)
 *
 * Returns NULL if called without NETC_BENCH_WITH_ZLIB defined, or on OOM.
 */

#ifndef BENCH_ZLIB_H
#define BENCH_ZLIB_H

#include "bench_compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a zlib adapter at the given compression level (1..9).
 * Returns NULL if zlib is not compiled in or on allocation failure.
 * Caller must call c->destroy(c) when done.
 */
bench_compressor_t *bench_zlib_create(int level);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_ZLIB_H */
