/**
 * bench_lz4.c — LZ4 compressor adapter implementation.
 */

#include "bench_lz4.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef NETC_BENCH_WITH_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif

/* =========================================================================
 * Adapter state
 * ========================================================================= */
typedef struct {
    bench_compressor_t base;
    int                hc;       /* 0=fast, 1=HC */
    char               name_buf[32];
    char               cfg_buf[32];
} lz4_adapter_t;

/* =========================================================================
 * vtable implementations
 * ========================================================================= */
static void lz4_reset(bench_compressor_t *c) { (void)c; }

static size_t lz4_compress(bench_compressor_t *c,
                            const uint8_t *src, size_t src_len,
                            uint8_t       *dst, size_t dst_cap)
{
#ifdef NETC_BENCH_WITH_LZ4
    lz4_adapter_t *a = (lz4_adapter_t *)c;
    int bound = LZ4_compressBound((int)src_len);
    if (bound <= 0 || (size_t)bound > dst_cap) return 0;

    int out_len;
    if (a->hc) {
        out_len = LZ4_compress_HC((const char *)src, (char *)dst,
                                  (int)src_len, (int)dst_cap, LZ4HC_CLEVEL_MAX);
    } else {
        out_len = LZ4_compress_default((const char *)src, (char *)dst,
                                       (int)src_len, (int)dst_cap);
    }
    return (out_len > 0) ? (size_t)out_len : 0;
#else
    (void)c; (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static size_t lz4_decompress(bench_compressor_t *c,
                              const uint8_t *src, size_t src_len,
                              uint8_t       *dst, size_t dst_cap)
{
#ifdef NETC_BENCH_WITH_LZ4
    (void)c;
    int out_len = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)src_len, (int)dst_cap);
    return (out_len > 0) ? (size_t)out_len : 0;
#else
    (void)c; (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static void lz4_destroy(bench_compressor_t *c)
{
    free(c);
}

/* =========================================================================
 * Factory
 * ========================================================================= */
bench_compressor_t *bench_lz4_create(int hc)
{
#ifndef NETC_BENCH_WITH_LZ4
    (void)hc;
    return NULL;
#else
    lz4_adapter_t *a = (lz4_adapter_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->hc = hc;
    if (hc) {
        snprintf(a->name_buf, sizeof(a->name_buf), "lz4-hc");
        snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "level=%d", LZ4HC_CLEVEL_MAX);
    } else {
        snprintf(a->name_buf, sizeof(a->name_buf), "lz4-fast");
        snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "default");
    }

    a->base.name       = a->name_buf;
    a->base.cfg        = a->cfg_buf;
    a->base.state      = a;
    a->base.train      = NULL;
    a->base.reset      = lz4_reset;
    a->base.compress   = lz4_compress;
    a->base.decompress = lz4_decompress;
    a->base.destroy    = lz4_destroy;

    return &a->base;
#endif
}
