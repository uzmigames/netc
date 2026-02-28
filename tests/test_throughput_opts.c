/**
 * test_throughput_opts.c — Tests for throughput optimisation changes.
 *
 * Covers:
 *   T.2  bucket_lut_matches_if_ladder  — LUT produces identical bucket
 *        indices to the original 16-if chain for all 65536 offsets.
 *   T.3  round_trip_after_opts — end-to-end compress/decompress still
 *        produces valid output after all Phase-1 and Phase-2 changes.
 */

#include "unity.h"
#include "../include/netc.h"
#include "../src/algo/netc_tans.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Reference implementation of the original 16-if ladder.
 * Keep in sync with what the LUT replaced.
 * ========================================================================= */
static uint32_t ref_ctx_bucket(uint32_t offset)
{
    if (offset <    8U) return  0U;
    if (offset <   16U) return  1U;
    if (offset <   24U) return  2U;
    if (offset <   32U) return  3U;
    if (offset <   48U) return  4U;
    if (offset <   64U) return  5U;
    if (offset <   96U) return  6U;
    if (offset <  128U) return  7U;
    if (offset <  192U) return  8U;
    if (offset <  256U) return  9U;
    if (offset <  384U) return 10U;
    if (offset <  512U) return 11U;
    if (offset < 1024U) return 12U;
    if (offset < 4096U) return 13U;
    if (offset <16384U) return 14U;
    return 15U;
}

/* =========================================================================
 * T.2 — LUT matches reference for all 65536 offsets
 * ========================================================================= */

void test_bucket_lut_matches_if_ladder_0_to_255(void)
{
    for (uint32_t off = 0; off < 256; off++) {
        uint32_t got = netc_ctx_bucket(off);
        uint32_t exp = ref_ctx_bucket(off);
        if (got != exp) {
            char msg[64];
            (void)snprintf(msg, sizeof(msg),
                "offset=%u: LUT=%u ref=%u", off, got, exp);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

void test_bucket_lut_matches_if_ladder_256_to_65535(void)
{
    for (uint32_t off = 256; off <= 65535; off++) {
        uint32_t got = netc_ctx_bucket(off);
        uint32_t exp = ref_ctx_bucket(off);
        if (got != exp) {
            char msg[64];
            (void)snprintf(msg, sizeof(msg),
                "offset=%u: LUT=%u ref=%u", off, got, exp);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

void test_bucket_boundaries_exact(void)
{
    /* Verify bucket transitions land exactly at expected offsets */
    TEST_ASSERT_EQUAL_UINT32(0U,  netc_ctx_bucket(0));
    TEST_ASSERT_EQUAL_UINT32(0U,  netc_ctx_bucket(7));
    TEST_ASSERT_EQUAL_UINT32(1U,  netc_ctx_bucket(8));
    TEST_ASSERT_EQUAL_UINT32(1U,  netc_ctx_bucket(15));
    TEST_ASSERT_EQUAL_UINT32(2U,  netc_ctx_bucket(16));
    TEST_ASSERT_EQUAL_UINT32(2U,  netc_ctx_bucket(23));
    TEST_ASSERT_EQUAL_UINT32(3U,  netc_ctx_bucket(24));
    TEST_ASSERT_EQUAL_UINT32(3U,  netc_ctx_bucket(31));
    TEST_ASSERT_EQUAL_UINT32(4U,  netc_ctx_bucket(32));
    TEST_ASSERT_EQUAL_UINT32(4U,  netc_ctx_bucket(47));
    TEST_ASSERT_EQUAL_UINT32(5U,  netc_ctx_bucket(48));
    TEST_ASSERT_EQUAL_UINT32(5U,  netc_ctx_bucket(63));
    TEST_ASSERT_EQUAL_UINT32(6U,  netc_ctx_bucket(64));
    TEST_ASSERT_EQUAL_UINT32(6U,  netc_ctx_bucket(95));
    TEST_ASSERT_EQUAL_UINT32(7U,  netc_ctx_bucket(96));
    TEST_ASSERT_EQUAL_UINT32(7U,  netc_ctx_bucket(127));
    TEST_ASSERT_EQUAL_UINT32(8U,  netc_ctx_bucket(128));
    TEST_ASSERT_EQUAL_UINT32(8U,  netc_ctx_bucket(191));
    TEST_ASSERT_EQUAL_UINT32(9U,  netc_ctx_bucket(192));
    TEST_ASSERT_EQUAL_UINT32(9U,  netc_ctx_bucket(255));
    TEST_ASSERT_EQUAL_UINT32(10U, netc_ctx_bucket(256));
    TEST_ASSERT_EQUAL_UINT32(10U, netc_ctx_bucket(383));
    TEST_ASSERT_EQUAL_UINT32(11U, netc_ctx_bucket(384));
    TEST_ASSERT_EQUAL_UINT32(11U, netc_ctx_bucket(511));
    TEST_ASSERT_EQUAL_UINT32(12U, netc_ctx_bucket(512));
    TEST_ASSERT_EQUAL_UINT32(12U, netc_ctx_bucket(1023));
    TEST_ASSERT_EQUAL_UINT32(13U, netc_ctx_bucket(1024));
    TEST_ASSERT_EQUAL_UINT32(13U, netc_ctx_bucket(4095));
    TEST_ASSERT_EQUAL_UINT32(14U, netc_ctx_bucket(4096));
    TEST_ASSERT_EQUAL_UINT32(14U, netc_ctx_bucket(16383));
    TEST_ASSERT_EQUAL_UINT32(15U, netc_ctx_bucket(16384));
    TEST_ASSERT_EQUAL_UINT32(15U, netc_ctx_bucket(65535));
}

/* =========================================================================
 * T.3 — Round-trip after all optimisation changes
 *
 * Helper: train a dict on repeated copies of pkt, then compress+decompress.
 * ========================================================================= */

static void do_roundtrip(const uint8_t *pkt, size_t pkt_sz,
                         uint32_t cfg_flags, int n_repeats)
{
    /* Build training corpus: n_repeats individual packets of pkt_sz bytes.
     * Pass each as a separate entry so the dict is trained on pkt_sz packets,
     * not on one huge concatenated buffer. */
    const uint8_t **pkts = (const uint8_t **)malloc((size_t)n_repeats * sizeof(const uint8_t *));
    size_t         *szs  = (size_t *)malloc((size_t)n_repeats * sizeof(size_t));
    TEST_ASSERT_NOT_NULL(pkts);
    TEST_ASSERT_NOT_NULL(szs);
    for (int i = 0; i < n_repeats; i++) { pkts[i] = pkt; szs[i] = pkt_sz; }

    netc_dict_t *dict = NULL;
    netc_result_t rc = netc_dict_train(pkts, szs, (size_t)n_repeats, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_NOT_NULL(dict);
    free(pkts);
    free(szs);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = cfg_flags;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t *comp   = (uint8_t *)malloc(pkt_sz + NETC_MAX_OVERHEAD + 16);
    uint8_t *decomp = (uint8_t *)malloc(pkt_sz);
    TEST_ASSERT_NOT_NULL(comp);
    TEST_ASSERT_NOT_NULL(decomp);

    size_t comp_sz = 0, decomp_sz = 0;

    rc = netc_compress(enc, pkt, pkt_sz, comp, pkt_sz + NETC_MAX_OVERHEAD + 16, &comp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);

    rc = netc_decompress(dec, comp, comp_sz, decomp, pkt_sz, &decomp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pkt_sz, (uint32_t)decomp_sz);
    TEST_ASSERT_EQUAL_MEMORY(pkt, decomp, pkt_sz);

    free(comp);
    free(decomp);
    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

void test_roundtrip_32B_compact(void)
{
    uint8_t pkt[32];
    for (int i = 0; i < 32; i++) pkt[i] = (uint8_t)(i * 5 + 3);
    do_roundtrip(pkt, 32,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR, 256);
}

void test_roundtrip_64B_compact(void)
{
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)(i * 3 + 7);
    do_roundtrip(pkt, 64,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR, 256);
}

void test_roundtrip_128B_compact(void)
{
    uint8_t pkt[128];
    for (int i = 0; i < 128; i++) pkt[i] = (uint8_t)(i ^ 0xAB);
    do_roundtrip(pkt, 128,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR, 128);
}

void test_roundtrip_256B_compact(void)
{
    /* 256B — spans 10 buckets; exercises two-candidate path (first + last) */
    uint8_t pkt[256];
    for (int i = 0; i < 256; i++) pkt[i] = (uint8_t)i;
    do_roundtrip(pkt, 256,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR, 64);
}

void test_roundtrip_512B_compact(void)
{
    uint8_t pkt[512];
    for (int i = 0; i < 512; i++) pkt[i] = (uint8_t)(i * 7 + 3);
    do_roundtrip(pkt, 512,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR, 32);
}

void test_roundtrip_with_delta(void)
{
    /* Two consecutive packets with DELTA enabled — verifies 2.2 removal
     * (delta-vs-LZP re-trial removed) still produces correct output. */
    uint8_t pkt0[64], pkt1[64];
    for (int i = 0; i < 64; i++) { pkt0[i] = (uint8_t)i; pkt1[i] = (uint8_t)(i + 1); }

    /* Train on pkt0 repeated */
    const uint8_t *pkts[] = { pkt0 };
    size_t         szs[]  = { 64 };
    netc_dict_t *dict = NULL;
    netc_result_t rc = netc_dict_train(pkts, szs, 1, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);

    uint32_t flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR
                   | NETC_CFG_FLAG_DELTA;
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = flags;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t comp[128], decomp[64];
    size_t  cs = 0, ds = 0;

    /* First packet */
    rc = netc_compress(enc, pkt0, 64, comp, sizeof(comp), &cs);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    rc = netc_decompress(dec, comp, cs, decomp, sizeof(decomp), &ds);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_MEMORY(pkt0, decomp, 64);

    /* Second packet — delta history from pkt0 */
    rc = netc_compress(enc, pkt1, 64, comp, sizeof(comp), &cs);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    rc = netc_decompress(dec, comp, cs, decomp, sizeof(decomp), &ds);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_MEMORY(pkt1, decomp, 64);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

/* =========================================================================
 * T.4 — FAST_COMPRESS roundtrip
 *
 * Verifies NETC_CFG_FLAG_FAST_COMPRESS produces fully decompressible output
 * for a range of packet sizes, with and without delta encoding.
 * The decompressor does NOT need the flag — compatibility is unconditional.
 * ========================================================================= */

void test_fast_compress_roundtrip_32B(void)
{
    uint8_t pkt[32];
    for (int i = 0; i < 32; i++) pkt[i] = (uint8_t)(i * 5 + 3);
    do_roundtrip(pkt, 32,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS, 256);
}

void test_fast_compress_roundtrip_64B(void)
{
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)(i * 3 + 7);
    do_roundtrip(pkt, 64,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS, 256);
}

void test_fast_compress_roundtrip_128B(void)
{
    uint8_t pkt[128];
    for (int i = 0; i < 128; i++) pkt[i] = (uint8_t)(i ^ 0xAB);
    do_roundtrip(pkt, 128,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS, 128);
}

void test_fast_compress_roundtrip_256B(void)
{
    uint8_t pkt[256];
    for (int i = 0; i < 256; i++) pkt[i] = (uint8_t)i;
    do_roundtrip(pkt, 256,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS, 64);
}

void test_fast_compress_roundtrip_512B(void)
{
    uint8_t pkt[512];
    for (int i = 0; i < 512; i++) pkt[i] = (uint8_t)(i * 7 + 3);
    do_roundtrip(pkt, 512,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS, 32);
}

void test_fast_compress_roundtrip_with_delta(void)
{
    /* FAST_COMPRESS + DELTA: skips LZP trial, uses PCTX directly */
    uint8_t pkt[128];
    for (int i = 0; i < 128; i++) pkt[i] = (uint8_t)(i * 2);
    do_roundtrip(pkt, 128,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA
        | NETC_CFG_FLAG_FAST_COMPRESS, 128);
}

void test_fast_compress_roundtrip_compact_hdr(void)
{
    /* FAST_COMPRESS + COMPACT_HDR: 2B header still valid for decompressor */
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)(i | 0x80u);
    do_roundtrip(pkt, 64,
        NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR
        | NETC_CFG_FLAG_FAST_COMPRESS, 256);
}

void test_fast_compress_decompressor_ignores_flag(void)
{
    /* Confirm that a context WITHOUT FAST_COMPRESS can decompress output
     * produced by a context WITH FAST_COMPRESS (flag is encode-only). */
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++) pkt[i] = (uint8_t)(i * 11 + 5);

    const uint8_t *pkts[] = { pkt };
    size_t         szs[]  = { 64 };
    netc_dict_t *dict = NULL;
    netc_result_t rc = netc_dict_train(pkts, szs, 1, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);

    uint32_t enc_flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_FAST_COMPRESS;
    uint32_t dec_flags = NETC_CFG_FLAG_STATEFUL; /* NO fast flag on decoder */

    netc_cfg_t enc_cfg, dec_cfg;
    memset(&enc_cfg, 0, sizeof(enc_cfg));
    memset(&dec_cfg, 0, sizeof(dec_cfg));
    enc_cfg.flags = enc_flags;
    dec_cfg.flags = dec_flags;

    netc_ctx_t *enc = netc_ctx_create(dict, &enc_cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &dec_cfg);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t comp[128], decomp[64];
    size_t cs = 0, ds = 0;

    rc = netc_compress(enc, pkt, 64, comp, sizeof(comp), &cs);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);

    rc = netc_decompress(dec, comp, cs, decomp, sizeof(decomp), &ds);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(64u, (uint32_t)ds);
    TEST_ASSERT_EQUAL_MEMORY(pkt, decomp, 64);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* T.2 — bucket LUT correctness */
    RUN_TEST(test_bucket_lut_matches_if_ladder_0_to_255);
    RUN_TEST(test_bucket_lut_matches_if_ladder_256_to_65535);
    RUN_TEST(test_bucket_boundaries_exact);

    /* T.3 — round-trip after optimisations */
    RUN_TEST(test_roundtrip_32B_compact);
    RUN_TEST(test_roundtrip_64B_compact);
    RUN_TEST(test_roundtrip_128B_compact);
    RUN_TEST(test_roundtrip_256B_compact);
    RUN_TEST(test_roundtrip_512B_compact);
    RUN_TEST(test_roundtrip_with_delta);

    /* T.4 — FAST_COMPRESS roundtrip */
    RUN_TEST(test_fast_compress_roundtrip_32B);
    RUN_TEST(test_fast_compress_roundtrip_64B);
    RUN_TEST(test_fast_compress_roundtrip_128B);
    RUN_TEST(test_fast_compress_roundtrip_256B);
    RUN_TEST(test_fast_compress_roundtrip_512B);
    RUN_TEST(test_fast_compress_roundtrip_with_delta);
    RUN_TEST(test_fast_compress_roundtrip_compact_hdr);
    RUN_TEST(test_fast_compress_decompressor_ignores_flag);

    return UNITY_END();
}
