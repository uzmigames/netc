/**
 * bench_netc.h — netc compressor adapter for the benchmark harness.
 *
 * Wraps netc_ctx_create / netc_compress / netc_decompress to conform to the
 * generic compressor interface used by the benchmark runner.
 *
 * Supports four configurations:
 *   BENCH_NETC_STATEFUL       — stateful, SIMD auto, delta on, no dict
 *   BENCH_NETC_STATEFUL_DICT  — stateful, trained dict, delta on
 *   BENCH_NETC_STATELESS      — stateless, no dict
 *   BENCH_NETC_STATELESS_DICT — stateless, trained dict
 */

#ifndef BENCH_NETC_H
#define BENCH_NETC_H

#include "bench_corpus.h"
#include "../include/netc.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Adapter handle
 * ========================================================================= */
typedef struct {
    netc_ctx_t  *enc_ctx;    /* Encoder context (NULL for stateless mode) */
    netc_ctx_t  *dec_ctx;    /* Decoder context (NULL for stateless mode) */
    netc_dict_t *dict;       /* NULL when no dictionary */
    int          stateless;
    uint32_t     flags;      /* saved cfg flags for re-init after train */
    uint8_t      simd_level;
    char         name[64];   /* human-readable config string */

    /* Scratch buffers (allocated once at init) */
    uint8_t     *comp_buf;
    size_t       comp_buf_cap;
} bench_netc_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Initialize a netc adapter.
 *
 * If 'dict' is non-NULL, the adapter takes ownership — do not free it
 * independently after this call; bench_netc_destroy will free it.
 *
 * flags:       NETC_CFG_FLAG_* bitmask (NETC_CFG_FLAG_STATEFUL/STATELESS + DELTA)
 * simd_level:  0=auto, 1=generic, 2=SSE4.2, 3=AVX2
 * max_pkt_size: maximum expected packet size (for scratch buffer allocation)
 */
int bench_netc_init(bench_netc_t *n,
                    netc_dict_t  *dict,
                    uint32_t      flags,
                    uint8_t       simd_level,
                    size_t        max_pkt_size);

/** Train and attach a dictionary from a workload corpus. */
int bench_netc_train(bench_netc_t *n,
                     bench_workload_t wl,
                     uint64_t seed,
                     size_t train_count);

/** Compress one packet. Returns compressed size, or 0 on error. */
size_t bench_netc_compress(bench_netc_t *n,
                           const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap);

/** Decompress one packet. Returns decompressed size, or 0 on error. */
size_t bench_netc_decompress(bench_netc_t *n,
                             const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_cap);

/** Reset per-connection state (for sequential packet series). */
void bench_netc_reset(bench_netc_t *n);

/** Free all resources. Safe to call on zero-initialized struct. */
void bench_netc_destroy(bench_netc_t *n);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_NETC_H */
