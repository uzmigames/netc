/**
 * bench_snappy.c — Optional Snappy compressor adapter.
 *
 * Guards all Snappy calls behind NETC_BENCH_WITH_SNAPPY.
 * When the library is absent, compress/decompress return 0 (skip).
 */

#include "bench_snappy.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Adapter state
 * ========================================================================= */

typedef struct {
    bench_compressor_t base;  /* MUST be first */
} snappy_state_t;

/* =========================================================================
 * vtable implementations
 * ========================================================================= */

static int snappy_train(bench_compressor_t *base,
                        bench_workload_t    wl,
                        uint64_t            seed,
                        size_t              n)
{
    (void)base; (void)wl; (void)seed; (void)n;
    return 0;  /* Snappy is a stateless compressor */
}

static void snappy_reset(bench_compressor_t *base)
{
    (void)base;
}

static size_t snappy_compress_fn(bench_compressor_t *base,
                                  const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap)
{
    (void)base;
#ifdef NETC_BENCH_WITH_SNAPPY
#ifdef __cplusplus
    /* snappy C++ API */
    size_t out_len = dst_cap;
    snappy_status status = snappy_compress((const char *)src, src_len,
                                            (char *)dst, &out_len);
    return (status == SNAPPY_OK) ? out_len : 0;
#else
    /* snappy-c C API */
    size_t out_len = dst_cap;
    snappy_status status = snappy_compress((const char *)src, src_len,
                                            (char *)dst, &out_len);
    return (status == SNAPPY_OK) ? out_len : 0;
#endif
#else
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static size_t snappy_decompress_fn(bench_compressor_t *base,
                                    const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap)
{
    (void)base;
#ifdef NETC_BENCH_WITH_SNAPPY
    size_t out_len = dst_cap;
    snappy_status status = snappy_uncompress((const char *)src, src_len,
                                              (char *)dst, &out_len);
    return (status == SNAPPY_OK) ? out_len : 0;
#else
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static void snappy_destroy(bench_compressor_t *base)
{
    free(base);
}

/* =========================================================================
 * Public constructor
 * ========================================================================= */

bench_compressor_t *bench_snappy_create(void)
{
    snappy_state_t *s = (snappy_state_t *)calloc(1, sizeof(snappy_state_t));
    if (!s) return NULL;

    s->base.name       = "snappy";
    s->base.cfg        = "snappy default";
    s->base.state      = s;
    s->base.train      = snappy_train;
    s->base.reset      = snappy_reset;
    s->base.compress   = snappy_compress_fn;
    s->base.decompress = snappy_decompress_fn;
    s->base.destroy    = snappy_destroy;

    return &s->base;
}
