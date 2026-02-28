/**
 * bench_snappy.h — Optional Snappy compressor adapter.
 *
 * Compiled only when NETC_BENCH_WITH_SNAPPY is defined (detected by CMake).
 * When the library is not available all operations return 0 (passthrough).
 *
 * Adapter name: "snappy"
 */

#ifndef BENCH_SNAPPY_H
#define BENCH_SNAPPY_H

#include "bench_compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a Snappy adapter.
 * train() is a no-op (Snappy is stateless).
 * Caller must call c->destroy(c) when done.
 */
bench_compressor_t *bench_snappy_create(void);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_SNAPPY_H */
