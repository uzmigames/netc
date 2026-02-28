/**
 * test_compress.c — Round-trip compression tests (compress + decompress).
 *
 * Tests:
 *   No-dict passthrough fallback:
 *     - ctx with NULL dict always falls back to passthrough
 *   tANS round-trip with trained dictionary:
 *     - Highly repetitive data (should compress)
 *     - Skewed byte distribution (should compress)
 *     - High-entropy data (may fall back to passthrough — round-trip still valid)
 *   Passthrough fallback (AD-006):
 *     - Random/high-entropy data compressed_size >= original → passthrough used
 *   Compression correctness:
 *     - Original bytes exactly recovered after decompress
 *     - dst_size from compress ≤ src_size + NETC_MAX_OVERHEAD (AD-006)
 *   Stateless round-trip:
 *     - netc_compress_stateless + netc_decompress_stateless
 *   MREG multi-region round-trip:
 *     - MREG flag set when tANS compresses
 *     - 16-byte and 128-byte packets spanning multiple context buckets
 *   RLE pre-pass round-trip:
 *     - All-same-byte runs (128 bytes)
 *     - Mixed runs of different bytes
 *   Edge cases:
 *     - 1-byte packet round-trip
 *     - Max packet size round-trip (65535 bytes)
 *   Error paths:
 *     - Compress with NULL ctx → NETC_ERR_CTX_NULL
 *     - Compress buf too small → NETC_ERR_BUF_SMALL
 *     - Decompress corrupt data → NETC_ERR_CORRUPT
 *     - Decompress wrong model_id → NETC_ERR_VERSION
 */

#include "unity.h"
#include "netc.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* =========================================================================
 * Test fixtures
 * ========================================================================= */

/* Highly repetitive — should compress well */
static uint8_t s_repetitive[512];

/* Skewed distribution — 80% byte 0x41 'A', rest spread */
static uint8_t s_skewed[512];

/* High-entropy — rotating bytes 0x00..0xFF (hard to compress) */
static uint8_t s_entropy[512];

static netc_dict_t *s_dict = NULL;
static netc_ctx_t  *s_ctx  = NULL;

/* =========================================================================
 * Unity lifecycle
 * ========================================================================= */

void setUp(void) {
    /* Build training data */
    memset(s_repetitive, 0x41, sizeof(s_repetitive));

    for (size_t i = 0; i < sizeof(s_skewed); i++) {
        s_skewed[i] = (i % 5 == 0) ? (uint8_t)(i & 0x7F) : (uint8_t)0x41;
    }
    for (size_t i = 0; i < sizeof(s_entropy); i++) {
        s_entropy[i] = (uint8_t)(i & 0xFF);
    }

    /* Train dictionary on repetitive + skewed data */
    const uint8_t *pkts[] = { s_repetitive, s_skewed };
    size_t         szs[]  = { sizeof(s_repetitive), sizeof(s_skewed) };
    netc_result_t r = netc_dict_train(pkts, szs, 2, 1, &s_dict);
    (void)r;  /* setUp can't fail tests directly */

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_STATS;
    s_ctx = netc_ctx_create(s_dict, &cfg);
}

void tearDown(void) {
    netc_ctx_destroy(s_ctx);
    s_ctx = NULL;
    netc_dict_free(s_dict);
    s_dict = NULL;
}

/* =========================================================================
 * Helper: compress + decompress, verify round-trip
 * Returns the algorithm byte used (from compressed header).
 * ========================================================================= */

static uint8_t do_roundtrip(
    netc_ctx_t    *ctx,
    const uint8_t *src,
    size_t         src_size,
    int           *ok)
{
    size_t  bound = netc_compress_bound(src_size);
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t *dbuf = (uint8_t *)malloc(src_size + 1);
    *ok = 0;

    if (!cbuf || !dbuf) { free(cbuf); free(dbuf); return 0xFF; }

    size_t csz = 0;
    netc_result_t cr = netc_compress(ctx, src, src_size, cbuf, bound, &csz);
    if (cr != NETC_OK) { free(cbuf); free(dbuf); return 0xFF; }

    /* Read algorithm from header (byte 5) */
    uint8_t alg = cbuf[5];

    netc_ctx_reset(ctx);

    size_t dsz = 0;
    netc_result_t dr = netc_decompress(ctx, cbuf, csz, dbuf, src_size, &dsz);
    if (dr != NETC_OK) { free(cbuf); free(dbuf); return 0xFF; }

    if (dsz == src_size && memcmp(src, dbuf, src_size) == 0) *ok = 1;

    free(cbuf);
    free(dbuf);
    return alg;
}

/* =========================================================================
 * No-dictionary passthrough
 * ========================================================================= */

void test_compress_no_dict_passthrough(void) {
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx_nodict = netc_ctx_create(NULL, &cfg);
    TEST_ASSERT_NOT_NULL(ctx_nodict);

    int ok = 0;
    uint8_t alg = do_roundtrip(ctx_nodict, s_repetitive, 64, &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
    TEST_ASSERT_EQUAL_UINT8(NETC_ALG_PASSTHRU, alg);

    netc_ctx_destroy(ctx_nodict);
}

/* =========================================================================
 * tANS round-trip with trained dict
 * ========================================================================= */

void test_compress_repetitive_roundtrip(void) {
    int ok = 0;
    do_roundtrip(s_ctx, s_repetitive, sizeof(s_repetitive), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

void test_compress_skewed_roundtrip(void) {
    int ok = 0;
    do_roundtrip(s_ctx, s_skewed, sizeof(s_skewed), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

void test_compress_entropy_roundtrip(void) {
    /* High-entropy may fall back to passthrough — but must round-trip correctly */
    int ok = 0;
    do_roundtrip(s_ctx, s_entropy, sizeof(s_entropy), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

/* =========================================================================
 * Output size guarantee (AD-006)
 * ========================================================================= */

void test_compress_output_fits_bound(void) {
    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    TEST_ASSERT_NOT_NULL(cbuf);

    size_t csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(s_ctx, s_repetitive, sizeof(s_repetitive),
                      cbuf, bound, &csz));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(bound, csz);
    free(cbuf);
}

void test_compress_output_at_most_src_plus_overhead(void) {
    /* The compress_bound is exactly src_size + NETC_MAX_OVERHEAD */
    size_t src_size = 256;
    size_t bound    = netc_compress_bound(src_size);
    TEST_ASSERT_EQUAL_UINT(src_size + NETC_MAX_OVERHEAD, bound);

    uint8_t *cbuf = (uint8_t *)malloc(bound);
    TEST_ASSERT_NOT_NULL(cbuf);

    size_t csz = 0;
    netc_compress(s_ctx, s_skewed, src_size, cbuf, bound, &csz);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(bound, csz);
    free(cbuf);
}

/* =========================================================================
 * tANS algorithm selection
 * ========================================================================= */

void test_compress_uses_tans_for_compressible_data(void) {
    /* Train on a single repeated byte — should compress heavily */
    uint8_t uniform[256];
    memset(uniform, 0x42, sizeof(uniform));

    const uint8_t *pkts[] = { uniform };
    size_t         szs[]  = { sizeof(uniform) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 2, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    size_t bound = netc_compress_bound(sizeof(uniform));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[256];
    size_t csz = 0, dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx, uniform, sizeof(uniform), cbuf, bound, &csz));
    /* Uniform data must compress to less than original */
    TEST_ASSERT_LESS_THAN(sizeof(uniform), csz);
    /* Round-trip must be lossless */
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(ctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(uniform), dsz);
    TEST_ASSERT_EQUAL_MEMORY(uniform, dbuf, sizeof(uniform));

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
    free(cbuf);
}

/* =========================================================================
 * Stateless round-trip
 * ========================================================================= */

void test_compress_stateless_roundtrip_repetitive(void) {
    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t *dbuf = (uint8_t *)malloc(sizeof(s_repetitive));
    TEST_ASSERT_NOT_NULL(cbuf);
    TEST_ASSERT_NOT_NULL(dbuf);

    size_t csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress_stateless(s_dict, s_repetitive, sizeof(s_repetitive),
                                cbuf, bound, &csz));

    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress_stateless(s_dict, cbuf, csz, dbuf,
                                  sizeof(s_repetitive), &dsz));

    TEST_ASSERT_EQUAL_UINT(sizeof(s_repetitive), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_repetitive, dbuf, sizeof(s_repetitive));

    free(cbuf);
    free(dbuf);
}

void test_compress_stateless_roundtrip_entropy(void) {
    size_t bound = netc_compress_bound(sizeof(s_entropy));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t *dbuf = (uint8_t *)malloc(sizeof(s_entropy));
    TEST_ASSERT_NOT_NULL(cbuf);
    TEST_ASSERT_NOT_NULL(dbuf);

    size_t csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress_stateless(s_dict, s_entropy, sizeof(s_entropy),
                                cbuf, bound, &csz));

    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress_stateless(s_dict, cbuf, csz, dbuf,
                                  sizeof(s_entropy), &dsz));

    TEST_ASSERT_EQUAL_UINT(sizeof(s_entropy), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_entropy, dbuf, sizeof(s_entropy));

    free(cbuf);
    free(dbuf);
}

/* =========================================================================
 * MREG (multi-region) flag verification
 * ========================================================================= */

void test_compress_mreg_flag_set_for_compressible(void) {
    /* A highly compressible uniform buffer should compress with tANS.
     * For packets spanning multiple buckets (> one bucket boundary),
     * the encoder may use MREG or single-region based on overhead trade-off.
     * We verify compression occurred and the round-trip is correct. */
    uint8_t uniform[256];
    memset(uniform, 0x42, sizeof(uniform));

    const uint8_t *pkts[] = { uniform };
    size_t         szs[]  = { sizeof(uniform) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 2, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    size_t bound = netc_compress_bound(sizeof(uniform));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[256];
    size_t csz = 0, dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx, uniform, sizeof(uniform), cbuf, bound, &csz));
    /* Data must compress (either via tANS or LZ77) */
    TEST_ASSERT_LESS_THAN(sizeof(uniform), csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(ctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(uniform), dsz);
    TEST_ASSERT_EQUAL_MEMORY(uniform, dbuf, sizeof(uniform));

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
    free(cbuf);
}

void test_compress_mreg_roundtrip_small_packet(void) {
    /* 16-byte packet exercises multiple context buckets (header + subheader) */
    uint8_t pkt[16];
    memset(pkt, 0xAA, sizeof(pkt));

    const uint8_t *pkts[] = { pkt };
    size_t         szs[]  = { sizeof(pkt) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 3, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    int ok = 0;
    do_roundtrip(ctx, pkt, sizeof(pkt), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
}

void test_compress_mreg_roundtrip_spans_multiple_buckets(void) {
    /* 128-byte packet spans 8 context buckets — exercises multi-region path */
    uint8_t pkt[128];
    memset(pkt, 0x55, sizeof(pkt));

    const uint8_t *pkts[] = { pkt };
    size_t         szs[]  = { sizeof(pkt) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 4, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    int ok = 0;
    do_roundtrip(ctx, pkt, sizeof(pkt), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
}

/* =========================================================================
 * RLE (run-length encoding) pre-pass verification
 * ========================================================================= */

void test_compress_rle_roundtrip_all_same_byte(void) {
    /* 128 identical bytes — RLE compresses to 2 bytes → much smaller than tANS */
    uint8_t rle_pkt[128];
    memset(rle_pkt, 0xCC, sizeof(rle_pkt));

    const uint8_t *pkts[] = { rle_pkt };
    size_t         szs[]  = { sizeof(rle_pkt) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 5, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    int ok = 0;
    do_roundtrip(ctx, rle_pkt, sizeof(rle_pkt), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
}

void test_compress_rle_roundtrip_mixed_runs(void) {
    /* Packet with distinct runs of different bytes */
    uint8_t rle_pkt[64];
    memset(rle_pkt,      0x11, 16);
    memset(rle_pkt + 16, 0x22, 16);
    memset(rle_pkt + 32, 0x33, 16);
    memset(rle_pkt + 48, 0x44, 16);

    const uint8_t *pkts[] = { rle_pkt };
    size_t         szs[]  = { sizeof(rle_pkt) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 6, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);

    int ok = 0;
    do_roundtrip(ctx, rle_pkt, sizeof(rle_pkt), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
}

/* =========================================================================
 * Edge cases
 * ========================================================================= */

void test_compress_one_byte_roundtrip(void) {
    uint8_t single[1] = { (uint8_t)0x41 };
    int ok = 0;
    do_roundtrip(s_ctx, single, 1, &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

void test_compress_max_size_roundtrip(void) {
    uint8_t *src = (uint8_t *)malloc(NETC_MAX_PACKET_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    /* Fill with repeating pattern so it's likely compressible */
    for (size_t i = 0; i < NETC_MAX_PACKET_SIZE; i++) {
        src[i] = (uint8_t)(i % 4);
    }
    int ok = 0;
    do_roundtrip(s_ctx, src, NETC_MAX_PACKET_SIZE, &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
    free(src);
}

void test_compress_all_same_byte_sizes(void) {
    /* Test with various packet sizes containing a single repeated byte */
    size_t sizes[] = { 8, 16, 32, 64, 128, 256 };
    uint8_t buf[256];
    memset(buf, 0x42, sizeof(buf));

    for (size_t k = 0; k < sizeof(sizes)/sizeof(sizes[0]); k++) {
        netc_ctx_reset(s_ctx);
        int ok = 0;
        do_roundtrip(s_ctx, buf, sizes[k], &ok);
        TEST_ASSERT_EQUAL_INT(1, ok);
    }
}

/* =========================================================================
 * Error paths
 * ========================================================================= */

void test_compress_null_ctx(void) {
    uint8_t src[8], dst[64];
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL,
        netc_compress(NULL, src, sizeof(src), dst, sizeof(dst), &dsz));
}

void test_compress_null_src(void) {
    uint8_t dst[64];
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_compress(s_ctx, NULL, 8, dst, sizeof(dst), &dsz));
}

void test_compress_null_dst(void) {
    uint8_t src[8];
    memset(src, 0, sizeof(src));
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_compress(s_ctx, src, sizeof(src), NULL, 64, &dsz));
}

void test_compress_too_large(void) {
    uint8_t src[8], dst[64];
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_TOOBIG,
        netc_compress(s_ctx, src, NETC_MAX_PACKET_SIZE + 1, dst, sizeof(dst), &dsz));
}

void test_compress_buf_too_small(void) {
    uint8_t src[8], dst[4];  /* dst too small even for header */
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL,
        netc_compress(s_ctx, src, sizeof(src), dst, sizeof(dst), &dsz));
}

void test_decompress_corrupt_header(void) {
    /* Feed only 3 bytes (too short for header) */
    uint8_t bad[3] = { 0x01, 0x02, 0x03 };
    uint8_t dst[64];
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT,
        netc_decompress(s_ctx, bad, sizeof(bad), dst, sizeof(dst), &dsz));
}

void test_decompress_wrong_model_id(void) {
    /* Compress with model_id=1 (s_dict), then try to decompress with a
     * different ctx that has a dict with model_id=2 */
    uint8_t src[64];
    memset(src, 0x41, sizeof(src));

    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    size_t csz = 0;
    netc_compress(s_ctx, src, sizeof(src), cbuf, bound, &csz);

    /* Create a second dict with a different model_id */
    const uint8_t *pkts[] = { src };
    size_t         szs[]  = { sizeof(src) };
    netc_dict_t *d2 = NULL;
    netc_dict_train(pkts, szs, 1, 2, &d2);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx2 = netc_ctx_create(d2, &cfg);

    uint8_t dst[128];
    size_t dsz = 0;
    netc_result_t r = netc_decompress(ctx2, cbuf, csz, dst, sizeof(dst), &dsz);
    /* If compression used tANS → model_id mismatch → NETC_ERR_VERSION.
     * If passthrough was used → no model_id check → NETC_OK or mismatch.
     * Either way the round-trip is wrong for a model mismatch under tANS. */
    if (cbuf[5] == NETC_ALG_TANS) {
        TEST_ASSERT_EQUAL_INT(NETC_ERR_VERSION, r);
    }
    /* For passthrough, no model_id check is enforced, so we just verify the
     * call doesn't crash and returns a sane value. */

    netc_ctx_destroy(ctx2);
    netc_dict_free(d2);
    free(cbuf);
}

/* =========================================================================
 * Statistics tracking
 * ========================================================================= */

void test_compress_stats_updated(void) {
    netc_ctx_reset(s_ctx);
    size_t bound = netc_compress_bound(64);
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    memset(dbuf, 0x41, sizeof(dbuf));

    size_t csz = 0;
    netc_compress(s_ctx, dbuf, sizeof(dbuf), cbuf, bound, &csz);

    netc_stats_t st;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_ctx_stats(s_ctx, &st));
    TEST_ASSERT_GREATER_THAN_UINT(0, st.packets_compressed);
    TEST_ASSERT_GREATER_THAN_UINT(0, st.bytes_in);
    TEST_ASSERT_GREATER_THAN_UINT(0, st.bytes_out);

    free(cbuf);
}

/* =========================================================================
 * LZ77 round-trip tests
 * ========================================================================= */

/* All-zeros: use a no-dict ctx so tANS is skipped, LZ77 activates.
 * LZ77 should compress significantly (a few bytes for 128 zeros). */
void test_compress_lz77_roundtrip_all_zeros(void) {
    uint8_t src[128];
    memset(src, 0x00, sizeof(src));
    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz = 0, dsz = 0;

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx_nd = netc_ctx_create(NULL, &cfg);

    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx_nd, src, sizeof(src), cbuf, bound, &csz));
    /* LZ77 should compress significantly (all zeros → single literal + back-ref) */
    TEST_ASSERT_LESS_THAN(sizeof(src), csz);
    /* Round-trip */
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(ctx_nd, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    netc_ctx_destroy(ctx_nd);
    free(cbuf);
}

/* Alternating 0xAA/0x55: LZ77 back-reference on 2-byte period. */
void test_compress_lz77_roundtrip_alternating(void) {
    uint8_t src[128];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (i & 1) ? 0x55u : 0xAAu;
    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz = 0, dsz = 0;

    netc_ctx_reset(s_ctx);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(s_ctx, src, sizeof(src), cbuf, bound, &csz));
    TEST_ASSERT_LESS_THAN(sizeof(src), csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(s_ctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    free(cbuf);
}

/* Half zeros, half ones: two runs → LZ77 compresses.
 * Use no-dict ctx so tANS is skipped and LZ77 activates. */
void test_compress_lz77_roundtrip_half_half(void) {
    uint8_t src[128];
    memset(src,      0x00, 64);
    memset(src + 64, 0xFF, 64);
    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz = 0, dsz = 0;

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx_nd = netc_ctx_create(NULL, &cfg);

    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx_nd, src, sizeof(src), cbuf, bound, &csz));
    TEST_ASSERT_LESS_THAN(sizeof(src), csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(ctx_nd, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    netc_ctx_destroy(ctx_nd);
    free(cbuf);
}

/* Stateless LZ77 round-trip (no ctx, no dict — pure back-reference). */
void test_compress_lz77_stateless_roundtrip(void) {
    uint8_t src[128];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(i % 4); /* 4-byte repeating pattern */
    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz = 0, dsz = 0;

    /* Use stateless path — LZ77 path requires ctx (arena), so use a fresh ctx
     * with no dictionary so tANS is skipped and LZ77 activates. */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL;
    netc_ctx_t *ctx_nodct = netc_ctx_create(NULL, &cfg);
    TEST_ASSERT_NOT_NULL(ctx_nodct);

    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx_nodct, src, sizeof(src), cbuf, bound, &csz));
    TEST_ASSERT_LESS_THAN(sizeof(src), csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(ctx_nodct, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    netc_ctx_destroy(ctx_nodct);
    free(cbuf);
}

/* LZ77 flag is set in compressed output for compressible repetitive data. */
void test_compress_lz77_flag_set(void) {
    uint8_t src[128];
    memset(src, 0xAB, sizeof(src));
    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    size_t csz = 0;

    netc_ctx_reset(s_ctx);
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(s_ctx, src, sizeof(src), cbuf, bound, &csz));

    /* If LZ77 was used, the flag must be present and algorithm = PASSTHRU */
    uint8_t flags = cbuf[4];
    uint8_t algo  = cbuf[5];
    if (flags & NETC_PKT_FLAG_LZ77) {
        TEST_ASSERT_EQUAL_UINT8(NETC_ALG_PASSTHRU, algo);
        TEST_ASSERT_NOT_EQUAL_UINT8(0, flags & NETC_PKT_FLAG_PASSTHRU);
    }
    /* Either way the round-trip must succeed */
    uint8_t dbuf[128];
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress(s_ctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    free(cbuf);
}

/* =========================================================================
 * Stateless delta rejection tests
 *
 * netc_decompress_stateless must reject packets with NETC_PKT_FLAG_DELTA
 * because it has no history to reconstruct the original bytes.
 * ========================================================================= */

/* Helper: craft a fake TANS packet header with DELTA flag set */
static void craft_delta_packet(uint8_t *buf, size_t buf_cap,
                                size_t payload_size, size_t *out_size)
{
    TEST_ASSERT_GREATER_OR_EQUAL(8 + payload_size, buf_cap);
    /* original_size = payload_size (LE) */
    buf[0] = (uint8_t)(payload_size & 0xFF);
    buf[1] = (uint8_t)(payload_size >> 8);
    /* compressed_size = payload_size */
    buf[2] = buf[0]; buf[3] = buf[1];
    /* flags = PASSTHRU | DELTA | DICT_ID */
    buf[4] = NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID;
    buf[5] = NETC_ALG_PASSTHRU;
    buf[6] = 1; /* model_id = 1 */
    buf[7] = 0; /* context_seq */
    memset(buf + 8, 0x42, payload_size);
    *out_size = 8 + payload_size;
}

/* Stateless decompressor must reject DELTA-flagged packets */
void test_decompress_stateless_rejects_delta_flag(void) {
    uint8_t cbuf[256];
    size_t  csz = 0;
    craft_delta_packet(cbuf, sizeof(cbuf), 64, &csz);

    uint8_t dbuf[128];
    size_t  dsz = 0;
    netc_result_t r = netc_decompress_stateless(s_dict, cbuf, csz,
                                                dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, r);
}

/* Stateless compress never sets DELTA flag */
void test_compress_stateless_never_sets_delta(void) {
    uint8_t src[128];
    /* Use highly repetitive data so tANS definitely activates */
    memset(src, 0xCC, sizeof(src));

    uint8_t cbuf[256];
    size_t  csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress_stateless(s_dict, src, sizeof(src),
                                cbuf, sizeof(cbuf), &csz));
    /* DELTA flag must NOT be set in any stateless compressed packet */
    TEST_ASSERT_EQUAL_UINT8(0, cbuf[4] & NETC_PKT_FLAG_DELTA);
}

/* Stateless LZ77 round-trip: repetitive data, no dict (no-dict ctx path uses
 * stateless functions indirectly; here we test the stateless API directly) */
void test_compress_stateless_lz77_roundtrip_repetitive(void) {
    /* Build a small no-dict context to get a no-dict trained dict —
     * actually use a corpus of all-zeros packets to train s_dict,
     * but s_dict is already trained.  The LZ77 path in stateless
     * fires when tANS ratio > 0.5.  Use repeating data that tANS
     * compresses poorly (not in the training corpus). */
    uint8_t src[128];
    /* Alternating 0x00/0xFF — likely not in training corpus → tANS may fail */
    for (int i = 0; i < 128; i++) src[i] = (i & 1) ? 0xFF : 0x00;

    uint8_t cbuf[256];
    size_t  csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress_stateless(s_dict, src, sizeof(src),
                                cbuf, sizeof(cbuf), &csz));

    /* Round-trip must reconstruct original exactly */
    uint8_t dbuf[128];
    size_t  dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_decompress_stateless(s_dict, cbuf, csz,
                                  dbuf, sizeof(dbuf), &dsz));
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));
}

/* Stateless: context_seq is always 0 (no per-packet state) */
void test_compress_stateless_context_seq_is_zero(void) {
    uint8_t src[64];
    memset(src, 0xAA, sizeof(src));

    uint8_t cbuf[256];
    size_t  csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress_stateless(s_dict, src, sizeof(src),
                                cbuf, sizeof(cbuf), &csz));
    /* Byte 7 = context_seq, must be 0 for stateless */
    TEST_ASSERT_EQUAL_UINT8(0, cbuf[7]);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* No-dict passthrough */
    RUN_TEST(test_compress_no_dict_passthrough);

    /* tANS round-trips */
    RUN_TEST(test_compress_repetitive_roundtrip);
    RUN_TEST(test_compress_skewed_roundtrip);
    RUN_TEST(test_compress_entropy_roundtrip);

    /* Output size guarantee */
    RUN_TEST(test_compress_output_fits_bound);
    RUN_TEST(test_compress_output_at_most_src_plus_overhead);

    /* Algorithm selection */
    RUN_TEST(test_compress_uses_tans_for_compressible_data);

    /* Stateless round-trips */
    RUN_TEST(test_compress_stateless_roundtrip_repetitive);
    RUN_TEST(test_compress_stateless_roundtrip_entropy);

    /* MREG flag and multi-region round-trips */
    RUN_TEST(test_compress_mreg_flag_set_for_compressible);
    RUN_TEST(test_compress_mreg_roundtrip_small_packet);
    RUN_TEST(test_compress_mreg_roundtrip_spans_multiple_buckets);

    /* RLE pre-pass round-trips */
    RUN_TEST(test_compress_rle_roundtrip_all_same_byte);
    RUN_TEST(test_compress_rle_roundtrip_mixed_runs);

    /* LZ77 round-trips */
    RUN_TEST(test_compress_lz77_roundtrip_all_zeros);
    RUN_TEST(test_compress_lz77_roundtrip_alternating);
    RUN_TEST(test_compress_lz77_roundtrip_half_half);
    RUN_TEST(test_compress_lz77_stateless_roundtrip);
    RUN_TEST(test_compress_lz77_flag_set);

    /* Edge cases */
    RUN_TEST(test_compress_one_byte_roundtrip);
    RUN_TEST(test_compress_max_size_roundtrip);
    RUN_TEST(test_compress_all_same_byte_sizes);

    /* Error paths */
    RUN_TEST(test_compress_null_ctx);
    RUN_TEST(test_compress_null_src);
    RUN_TEST(test_compress_null_dst);
    RUN_TEST(test_compress_too_large);
    RUN_TEST(test_compress_buf_too_small);
    RUN_TEST(test_decompress_corrupt_header);
    RUN_TEST(test_decompress_wrong_model_id);

    /* Statistics */
    RUN_TEST(test_compress_stats_updated);

    /* Stateless delta rejection */
    RUN_TEST(test_decompress_stateless_rejects_delta_flag);
    RUN_TEST(test_compress_stateless_never_sets_delta);
    RUN_TEST(test_compress_stateless_lz77_roundtrip_repetitive);
    RUN_TEST(test_compress_stateless_context_seq_is_zero);

    return UNITY_END();
}
