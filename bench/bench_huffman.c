/**
 * bench_huffman.c — Reference static Huffman adapter using zlib Z_HUFFMAN_ONLY.
 *
 * zlib's compress2 with strategy=Z_HUFFMAN_ONLY performs byte-level Huffman
 * coding without LZ77, making it a fair "pure entropy coder" reference point.
 * This is always available when zlib is present (same dep as bench_zlib.c).
 *
 * When zlib is not available, all ops return 0 (skip).
 * Adapter name: "huffman-static"
 */

#include "bench_huffman.h"
#include <stdlib.h>
#include <string.h>

#ifdef NETC_BENCH_WITH_ZLIB
#  include <zlib.h>
#endif

typedef struct {
    bench_compressor_t base;  /* MUST be first */
} huf_state_t;

static int huf_train(bench_compressor_t *base,
                     bench_workload_t wl, uint64_t seed, size_t n)
{
    (void)base; (void)wl; (void)seed; (void)n;
    return 0;  /* zlib Huffman-only is stateless */
}

static void huf_reset(bench_compressor_t *base) { (void)base; }

static size_t huf_compress_fn(bench_compressor_t *base,
                               const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    (void)base;
#ifdef NETC_BENCH_WITH_ZLIB
    uLongf out_len = (uLongf)dst_cap;
    /* Z_HUFFMAN_ONLY: Huffman coding, no LZ77 string matching */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                     15, 8, Z_HUFFMAN_ONLY) != Z_OK) return 0;
    zs.next_in   = (Bytef *)src;
    zs.avail_in  = (uInt)src_len;
    zs.next_out  = (Bytef *)dst;
    zs.avail_out = (uInt)dst_cap;
    int rc = deflate(&zs, Z_FINISH);
    out_len = zs.total_out;
    deflateEnd(&zs);
    return (rc == Z_STREAM_END) ? (size_t)out_len : 0;
#else
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static size_t huf_decompress_fn(bench_compressor_t *base,
                                 const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_cap)
{
    (void)base;
#ifdef NETC_BENCH_WITH_ZLIB
    uLongf out_len = (uLongf)dst_cap;
    int rc = uncompress((Bytef *)dst, &out_len,
                        (const Bytef *)src, (uLong)src_len);
    return (rc == Z_OK) ? (size_t)out_len : 0;
#else
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
#endif
}

static void huf_destroy(bench_compressor_t *base) { free(base); }

bench_compressor_t *bench_huffman_create(void)
{
    huf_state_t *s = (huf_state_t *)calloc(1, sizeof(huf_state_t));
    if (!s) return NULL;

    s->base.name       = "huffman-static";
    s->base.cfg        = "zlib Z_HUFFMAN_ONLY";
    s->base.state      = s;
    s->base.train      = huf_train;
    s->base.reset      = huf_reset;
    s->base.compress   = huf_compress_fn;
    s->base.decompress = huf_decompress_fn;
    s->base.destroy    = huf_destroy;

    return &s->base;
}
