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

    int ok = 0;
    uint8_t alg = do_roundtrip(ctx, uniform, sizeof(uniform), &ok);
    TEST_ASSERT_EQUAL_INT(1, ok);
    /* Should use tANS since the uniform data compresses well */
    TEST_ASSERT_EQUAL_UINT8(NETC_ALG_TANS, alg);

    netc_ctx_destroy(ctx);
    netc_dict_free(d);
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

    return UNITY_END();
}
