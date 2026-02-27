/**
 * bench_netc.c — netc compressor adapter.
 *
 * Uses separate encoder and decoder contexts so that stateful delta
 * prediction state does not get corrupted by interleaved compress/decompress
 * calls in the benchmark roundtrip loop.
 */

#include "bench_netc.h"
#include "bench_corpus.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal: create enc+dec context pair
 * ========================================================================= */
static int create_ctx_pair(bench_netc_t *n)
{
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags      = n->flags;
    cfg.simd_level = n->simd_level;

    n->enc_ctx = netc_ctx_create(n->dict, &cfg);
    n->dec_ctx = netc_ctx_create(n->dict, &cfg);
    if (!n->enc_ctx || !n->dec_ctx) {
        netc_ctx_destroy(n->enc_ctx); n->enc_ctx = NULL;
        netc_ctx_destroy(n->dec_ctx); n->dec_ctx = NULL;
        return -1;
    }
    return 0;
}

/* =========================================================================
 * bench_netc_init
 * ========================================================================= */
int bench_netc_init(bench_netc_t *n,
                    netc_dict_t  *dict,
                    uint32_t      flags,
                    uint8_t       simd_level,
                    size_t        max_pkt_size)
{
    if (!n) return -1;
    memset(n, 0, sizeof(*n));

    n->dict       = dict;
    n->flags      = flags;
    n->simd_level = simd_level;
    n->stateless  = (flags & NETC_CFG_FLAG_STATELESS) ? 1 : 0;

    /* Scratch buffer for compressed output */
    size_t cap = max_pkt_size + NETC_MAX_OVERHEAD + 64u;
    n->comp_buf = (uint8_t *)malloc(cap);
    if (!n->comp_buf) return -1;
    n->comp_buf_cap = cap;

    /* Build context name */
    const char *mode  = n->stateless ? "stateless" : "stateful";
    const char *delta = (flags & NETC_CFG_FLAG_DELTA) ? "+delta" : "";
    const char *dct   = dict ? "+dict" : "";
    snprintf(n->name, sizeof(n->name), "netc/%s%s%s simd=%u",
             mode, delta, dct, simd_level);

    /* Create enc+dec context pair for stateful mode */
    if (!n->stateless) {
        if (create_ctx_pair(n) != 0) {
            free(n->comp_buf);
            n->comp_buf = NULL;
            return -1;
        }
    }

    return 0;
}

/* =========================================================================
 * bench_netc_train
 * ========================================================================= */
int bench_netc_train(bench_netc_t *n,
                     bench_workload_t wl,
                     uint64_t seed,
                     size_t train_count)
{
    if (!n) return -1;

    /* Free old dictionary */
    if (n->dict) {
        netc_dict_free(n->dict);
        n->dict = NULL;
    }

    /* Allocate corpus storage */
    uint8_t **bufs    = (uint8_t **)malloc(train_count * sizeof(uint8_t *));
    size_t   *lens    = (size_t   *)malloc(train_count * sizeof(size_t));
    uint8_t  *storage = (uint8_t  *)malloc(train_count * BENCH_CORPUS_MAX_PKT);
    if (!bufs || !lens || !storage) {
        free(bufs); free(lens); free(storage);
        return -1;
    }

    bench_corpus_train(wl, seed, bufs, lens, train_count, storage);

    netc_dict_t *dict = NULL;
    netc_result_t rc = netc_dict_train(
        (const uint8_t * const *)bufs, lens, train_count, 1, &dict);

    free(bufs); free(lens); free(storage);

    if (rc != NETC_OK || !dict) return -1;

    n->dict = dict;

    /* Re-create enc+dec context pair with new dictionary */
    if (!n->stateless) {
        netc_ctx_destroy(n->enc_ctx); n->enc_ctx = NULL;
        netc_ctx_destroy(n->dec_ctx); n->dec_ctx = NULL;
        if (create_ctx_pair(n) != 0) return -1;
    }

    /* Update name to reflect dict */
    const char *mode  = n->stateless ? "stateless" : "stateful";
    const char *delta = (n->flags & NETC_CFG_FLAG_DELTA) ? "+delta" : "";
    snprintf(n->name, sizeof(n->name), "netc/%s%s+dict simd=%u",
             mode, delta, (unsigned)n->simd_level);

    return 0;
}

/* =========================================================================
 * bench_netc_compress
 * ========================================================================= */
size_t bench_netc_compress(bench_netc_t *n,
                           const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap)
{
    if (!n || !src || !dst) return 0;

    size_t out_size = 0;
    netc_result_t rc;

    if (n->stateless) {
        if (!n->dict) return 0;
        rc = netc_compress_stateless(n->dict, src, src_len, dst, dst_cap, &out_size);
    } else {
        if (!n->enc_ctx) return 0;
        rc = netc_compress(n->enc_ctx, src, src_len, dst, dst_cap, &out_size);
    }

    return (rc == NETC_OK) ? out_size : 0;
}

/* =========================================================================
 * bench_netc_decompress
 * ========================================================================= */
size_t bench_netc_decompress(bench_netc_t *n,
                             const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_cap)
{
    if (!n || !src || !dst) return 0;

    size_t out_size = 0;
    netc_result_t rc;

    if (n->stateless) {
        if (!n->dict) return 0;
        rc = netc_decompress_stateless(n->dict, src, src_len, dst, dst_cap, &out_size);
    } else {
        if (!n->dec_ctx) return 0;
        rc = netc_decompress(n->dec_ctx, src, src_len, dst, dst_cap, &out_size);
    }

    return (rc == NETC_OK) ? out_size : 0;
}

/* =========================================================================
 * bench_netc_reset
 * ========================================================================= */
void bench_netc_reset(bench_netc_t *n)
{
    if (!n) return;
    if (n->enc_ctx) netc_ctx_reset(n->enc_ctx);
    if (n->dec_ctx) netc_ctx_reset(n->dec_ctx);
}

/* =========================================================================
 * bench_netc_destroy
 * ========================================================================= */
void bench_netc_destroy(bench_netc_t *n)
{
    if (!n) return;
    if (n->enc_ctx)  { netc_ctx_destroy(n->enc_ctx); n->enc_ctx = NULL; }
    if (n->dec_ctx)  { netc_ctx_destroy(n->dec_ctx); n->dec_ctx = NULL; }
    if (n->dict)     { netc_dict_free(n->dict);       n->dict    = NULL; }
    if (n->comp_buf) { free(n->comp_buf);              n->comp_buf = NULL; }
}
