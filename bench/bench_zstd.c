/**
 * bench_zstd.c — Zstd compressor adapter implementation.
 */

#include "bench_zstd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef NETC_BENCH_WITH_ZSTD
#include <zstd.h>
#include <zdict.h>
#endif

/* =========================================================================
 * Adapter state
 * ========================================================================= */
typedef struct {
    bench_compressor_t base;
    int                level;
    int                use_dict;
    char               name_buf[48];
    char               cfg_buf[48];

#ifdef NETC_BENCH_WITH_ZSTD
    ZSTD_CCtx         *cctx;
    ZSTD_DCtx         *dctx;
    ZSTD_CDict        *cdict;
    ZSTD_DDict        *ddict;
    void              *dict_buf;
    size_t             dict_size;
#endif
} zstd_adapter_t;

/* =========================================================================
 * vtable
 * ========================================================================= */
static void zstd_reset(bench_compressor_t *c) { (void)c; }

#ifdef NETC_BENCH_WITH_ZSTD

static int zstd_train(bench_compressor_t *c,
                      bench_workload_t wl, uint64_t seed, size_t n)
{
    zstd_adapter_t *a = (zstd_adapter_t *)c;
    if (!a->use_dict) return 0;

    /* Collect n training samples */
    size_t max_pkt = BENCH_CORPUS_MAX_PKT;
    size_t buf_cap = n * max_pkt;
    uint8_t *sample_buf = (uint8_t *)malloc(buf_cap);
    size_t  *sample_sizes = (size_t *)malloc(n * sizeof(size_t));
    if (!sample_buf || !sample_sizes) {
        free(sample_buf); free(sample_sizes);
        return -1;
    }

    bench_corpus_t corpus;
    bench_corpus_init(&corpus, wl, seed);
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) { bench_corpus_reset(&corpus); plen = bench_corpus_next(&corpus); }
        if (plen == 0 || total + plen > buf_cap) { sample_sizes[i] = 0; continue; }
        memcpy(sample_buf + total, corpus.packet, plen);
        sample_sizes[i] = plen;
        total += plen;
    }

    /* Train Zstd dictionary (target size 112 KB — Zstd default) */
    size_t dict_cap = 112 * 1024u;
    void  *dict_buf = malloc(dict_cap);
    if (!dict_buf) { free(sample_buf); free(sample_sizes); return -1; }

    size_t dict_size = ZDICT_trainFromBuffer(dict_buf, dict_cap,
                                             sample_buf, sample_sizes, (unsigned)n);
    free(sample_buf);
    free(sample_sizes);

    if (ZSTD_isError(dict_size)) {
        free(dict_buf);
        /* Fall back to no dictionary — not fatal */
        fprintf(stderr, "[bench_zstd] Dict training failed: %s — running without dict\n",
                ZSTD_getErrorName(dict_size));
        a->use_dict = 0;
        return 0;
    }

    a->dict_buf  = dict_buf;
    a->dict_size = dict_size;
    a->cdict = ZSTD_createCDict(dict_buf, dict_size, a->level);
    a->ddict = ZSTD_createDDict(dict_buf, dict_size);

    if (!a->cdict || !a->ddict) {
        ZSTD_freeCDict(a->cdict); a->cdict = NULL;
        ZSTD_freeDDict(a->ddict); a->ddict = NULL;
        free(dict_buf); a->dict_buf = NULL;
        a->use_dict = 0;
    }

    /* Update name to reflect dict */
    snprintf(a->name_buf, sizeof(a->name_buf), "zstd-%d-dict", a->level);
    snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "level=%d+dict(%zu B)",
             a->level, a->dict_size);

    return 0;
}

static size_t zstd_compress(bench_compressor_t *c,
                             const uint8_t *src, size_t src_len,
                             uint8_t       *dst, size_t dst_cap)
{
    zstd_adapter_t *a = (zstd_adapter_t *)c;
    size_t out_len;
    if (a->cdict) {
        out_len = ZSTD_compress_usingCDict(a->cctx,
                                           dst, dst_cap,
                                           src, src_len,
                                           a->cdict);
    } else {
        out_len = ZSTD_compressCCtx(a->cctx,
                                    dst, dst_cap,
                                    src, src_len,
                                    a->level);
    }
    return ZSTD_isError(out_len) ? 0 : out_len;
}

static size_t zstd_decompress(bench_compressor_t *c,
                               const uint8_t *src, size_t src_len,
                               uint8_t       *dst, size_t dst_cap)
{
    zstd_adapter_t *a = (zstd_adapter_t *)c;
    size_t out_len;
    if (a->ddict) {
        out_len = ZSTD_decompress_usingDDict(a->dctx,
                                             dst, dst_cap,
                                             src, src_len,
                                             a->ddict);
    } else {
        out_len = ZSTD_decompressDCtx(a->dctx,
                                      dst, dst_cap,
                                      src, src_len);
    }
    return ZSTD_isError(out_len) ? 0 : out_len;
}

#else /* !NETC_BENCH_WITH_ZSTD */

static int    zstd_train(bench_compressor_t *c, bench_workload_t wl,
                         uint64_t s, size_t n)
{ (void)c;(void)wl;(void)s;(void)n; return 0; }
static size_t zstd_compress(bench_compressor_t *c, const uint8_t *src,
                             size_t src_len, uint8_t *dst, size_t dst_cap)
{ (void)c;(void)src;(void)src_len;(void)dst;(void)dst_cap; return 0; }
static size_t zstd_decompress(bench_compressor_t *c, const uint8_t *src,
                               size_t src_len, uint8_t *dst, size_t dst_cap)
{ (void)c;(void)src;(void)src_len;(void)dst;(void)dst_cap; return 0; }

#endif /* NETC_BENCH_WITH_ZSTD */

static void zstd_destroy(bench_compressor_t *c)
{
#ifdef NETC_BENCH_WITH_ZSTD
    zstd_adapter_t *a = (zstd_adapter_t *)c;
    ZSTD_freeCCtx(a->cctx);
    ZSTD_freeDCtx(a->dctx);
    ZSTD_freeCDict(a->cdict);
    ZSTD_freeDDict(a->ddict);
    free(a->dict_buf);
#endif
    free(c);
}

/* =========================================================================
 * Factory
 * ========================================================================= */
bench_compressor_t *bench_zstd_create(int level, int use_dict)
{
#ifndef NETC_BENCH_WITH_ZSTD
    (void)level; (void)use_dict;
    return NULL;
#else
    zstd_adapter_t *a = (zstd_adapter_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->level    = level;
    a->use_dict = use_dict;

    if (use_dict) {
        snprintf(a->name_buf, sizeof(a->name_buf), "zstd-%d-dict", level);
        snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "level=%d+dict(pending)", level);
    } else {
        snprintf(a->name_buf, sizeof(a->name_buf), "zstd-%d", level);
        snprintf(a->cfg_buf,  sizeof(a->cfg_buf),  "level=%d", level);
    }

    a->cctx = ZSTD_createCCtx();
    a->dctx = ZSTD_createDCtx();
    if (!a->cctx || !a->dctx) {
        ZSTD_freeCCtx(a->cctx);
        ZSTD_freeDCtx(a->dctx);
        free(a);
        return NULL;
    }

    a->base.name       = a->name_buf;
    a->base.cfg        = a->cfg_buf;
    a->base.state      = a;
    a->base.train      = use_dict ? zstd_train : NULL;
    a->base.reset      = zstd_reset;
    a->base.compress   = zstd_compress;
    a->base.decompress = zstd_decompress;
    a->base.destroy    = zstd_destroy;

    return &a->base;
#endif
}
