/**
 * bench_lz4.h — LZ4 compressor adapter.
 *
 * Compiled only when NETC_BENCH_WITH_LZ4 is defined (LZ4 detected by CMake).
 * Provides two modes:
 *   bench_lz4_create(0)  — LZ4_compress_default (fast mode)
 *   bench_lz4_create(1)  — LZ4_compress_HC (high-compression mode)
 *
 * Returns NULL if LZ4 is not compiled in or on allocation failure.
 * Caller must call c->destroy(c) when done.
 */

#ifndef BENCH_LZ4_H
#define BENCH_LZ4_H

#include "bench_compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an LZ4 adapter.
 * hc=0 → LZ4 fast (LZ4_compress_default)
 * hc=1 → LZ4 HC   (LZ4_compress_HC, level=9)
 */
bench_compressor_t *bench_lz4_create(int hc);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_LZ4_H */
