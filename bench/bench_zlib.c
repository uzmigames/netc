/**
 * bench_zlib.c — zlib compressor adapter implementation.
 *
 * Compiled unconditionally; operations return 0 gracefully when
 * NETC_BENCH_WITH_ZLIB is not defined (stub mode).
 */

#include "bench_zlib.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef NETC_BENCH_WITH_ZLIB
#include <zlib.h>
#endif

/* =========================================================================
 * Adapter state
 * ========================================================================= */
typedef struct {
    bench_compressor_t base;  /* MUST be first — vtable + name/cfg */
    int                level;
    char               name_buf[32];
    char               cfg_buf[32];
} zlib_adapter_t;

/* =========================================================================
 * vtable implementations
 * ========================================================================= */
static void zlib_reset(bench_compressor_t *c) { (void)c; /* stateless */ }

static size_t zlib_compress(bench_compressor_t *c,
                             const uint8_t *src, size_t src_len,
                             uint8_t       *dst, size_t dst_cap)
{
#ifdef NETC_BENCH_WITH_ZLIB
    zlib_adapter_t *a = (zlib_adapter_t *)c;
    uLongf out_len = (uLongf)dst_cap;
    int rc = compress2((Bytef *)dst, &out_len,
                       (const Bytef *)src, (uLong)src_len,
                       a->level);
    if (rc != Z_OK) return 0;
    return (size_t)out_len;
#else
    (void)c; (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static size_t zlib_decompress(bench_compressor_t *c,
                               const uint8_t *src, size_t src_len,
                               uint8_t       *dst, size_t dst_cap)
{
#ifdef NETC_BENCH_WITH_ZLIB
    (void)c;
    uLongf out_len = (uLongf)dst_cap;
    int rc = uncompress((Bytef *)dst, &out_len,
                        (const Bytef *)src, (uLong)src_len);
    if (rc != Z_OK) return 0;
    return (size_t)out_len;
#else
    (void)c; (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static void zlib_destroy(bench_compressor_t *c)
{
    free(c);
}

/* =========================================================================
 * Factory
 * ========================================================================= */
bench_compressor_t *bench_zlib_create(int level)
{
#ifndef NETC_BENCH_WITH_ZLIB
    (void)level;
    return NULL;
#else
    zlib_adapter_t *a = (zlib_adapter_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->level = level;
    snprintf(a->name_buf, sizeof(a->name_buf), "zlib-%d", level);
    snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "level=%d", level);

    a->base.name       = a->name_buf;
    a->base.cfg        = a->cfg_buf;
    a->base.state      = a;
    a->base.train      = NULL;
    a->base.reset      = zlib_reset;
    a->base.compress   = zlib_compress;
    a->base.decompress = zlib_decompress;
    a->base.destroy    = zlib_destroy;

    return &a->base;
#endif
}
