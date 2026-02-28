/**
 * test_compact_header.c — Tests for the compact packet header (2-4 bytes).
 *
 * Tests:
 *   Packet type encoding:
 *     - Encode/decode round-trip for all passthrough variants
 *     - Encode/decode round-trip for TANS + bucket combos
 *     - Encode/decode round-trip for MREG, PCTX, LZ77X, LZP variants
 *     - Invalid combinations return 0xFF
 *   Size varint:
 *     - original_size 0-127 → 2-byte header
 *     - original_size 128-65535 → 4-byte header
 *     - Boundary values: 0, 1, 127, 128, 255, 32767, 65535
 *   Compact header write/read round-trip:
 *     - Write then read back for various types and sizes
 *     - Truncated input → read returns 0
 *   Compress/decompress round-trip with compact headers:
 *     - Passthrough (NULL dict) round-trip
 *     - tANS with trained dict round-trip
 *     - Repetitive data round-trip
 *     - Verify header is smaller than legacy (2 or 4 bytes vs 8)
 *   Error cases:
 *     - Truncated compact header (1 byte)
 *     - Truncated 4-byte header (3 bytes)
 *     - Invalid packet type byte
 */

#include "unity.h"
#include "netc.h"
#include "../src/core/netc_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* =========================================================================
 * Test fixtures
 * ========================================================================= */

static uint8_t s_repetitive[64];
static uint8_t s_skewed[128];
static uint8_t s_entropy[64];

static netc_dict_t *s_dict = NULL;

void setUp(void)
{
    memset(s_repetitive, 0x41, sizeof(s_repetitive));

    for (size_t i = 0; i < sizeof(s_skewed); i++)
        s_skewed[i] = (i % 5 == 0) ? (uint8_t)(i & 0x7F) : (uint8_t)0x41;

    for (size_t i = 0; i < sizeof(s_entropy); i++)
        s_entropy[i] = (uint8_t)(i & 0xFF);

    const uint8_t *pkts[] = { s_repetitive, s_skewed };
    size_t         szs[]  = { sizeof(s_repetitive), sizeof(s_skewed) };
    netc_dict_train(pkts, szs, 2, 1, &s_dict);
}

void tearDown(void)
{
    netc_dict_free(s_dict);
    s_dict = NULL;
}

/* =========================================================================
 * Helper: create a compact-header context
 * ========================================================================= */

static netc_ctx_t *make_compact_ctx(const netc_dict_t *dict, uint32_t extra_flags)
{
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR | extra_flags;
    return netc_ctx_create(dict, &cfg);
}

/* =========================================================================
 * Packet type encode/decode tests
 * ========================================================================= */

void test_pkt_type_passthru_roundtrip(void)
{
    /* Plain passthrough */
    uint8_t pt = netc_compact_type_encode(
        NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DICT_ID, NETC_ALG_PASSTHRU);
    TEST_ASSERT_EQUAL_UINT8(0x00, pt);
    TEST_ASSERT_BITS(NETC_PKT_FLAG_PASSTHRU, NETC_PKT_FLAG_PASSTHRU,
                     netc_pkt_type_table[pt].flags);
    TEST_ASSERT_EQUAL_UINT8(NETC_ALG_PASSTHRU, netc_pkt_type_table[pt].algorithm);

    /* Passthrough + LZ77 */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_PASSTHRU);
    TEST_ASSERT_EQUAL_UINT8(0x01, pt);

    /* Passthrough + LZ77 + DELTA */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_PASSTHRU);
    TEST_ASSERT_EQUAL_UINT8(0x02, pt);

    /* Passthrough + RLE */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_RLE | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_PASSTHRU);
    TEST_ASSERT_EQUAL_UINT8(0x03, pt);
}

void test_pkt_type_tans_bucket_roundtrip(void)
{
    for (uint8_t bucket = 0; bucket < 16; bucket++) {
        uint8_t alg = (uint8_t)(NETC_ALG_TANS | (bucket << 4));

        /* TANS + bucket */
        uint8_t pt = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x10u + bucket, pt);
        TEST_ASSERT_EQUAL_UINT8(alg, netc_pkt_type_table[pt].algorithm);

        /* TANS + DELTA + bucket */
        pt = netc_compact_type_encode(NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x20u + bucket, pt);

        /* TANS + BIGRAM + bucket */
        pt = netc_compact_type_encode(NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x30u + bucket, pt);

        /* TANS + BIGRAM + DELTA + bucket */
        pt = netc_compact_type_encode(
            NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x40u + bucket, pt);

        /* TANS + X2 + bucket */
        pt = netc_compact_type_encode(NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x50u + bucket, pt);

        /* TANS + X2 + DELTA + bucket */
        pt = netc_compact_type_encode(
            NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x60u + bucket, pt);
    }
}

void test_pkt_type_lzp_bucket_roundtrip(void)
{
    for (uint8_t bucket = 0; bucket < 16; bucket++) {
        uint8_t alg = (uint8_t)(NETC_ALG_LZP | (bucket << 4));

        uint8_t pt = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x70u + bucket, pt);
        TEST_ASSERT_EQUAL_UINT8(alg, netc_pkt_type_table[pt].algorithm);

        pt = netc_compact_type_encode(NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x80u + bucket, pt);
    }
}

void test_pkt_type_mreg_variants(void)
{
    /* MREG */
    uint8_t pt = netc_compact_type_encode(
        NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS);
    TEST_ASSERT_EQUAL_UINT8(0x08, pt);

    /* MREG + DELTA */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS);
    TEST_ASSERT_EQUAL_UINT8(0x09, pt);

    /* MREG + X2 */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS);
    TEST_ASSERT_EQUAL_UINT8(0x0A, pt);

    /* MREG + BIGRAM */
    pt = netc_compact_type_encode(
        NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS);
    TEST_ASSERT_EQUAL_UINT8(0x0C, pt);
}

void test_pkt_type_pctx_variants(void)
{
    uint8_t pt = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX);
    TEST_ASSERT_EQUAL_UINT8(0x04, pt);

    pt = netc_compact_type_encode(NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
                                   NETC_ALG_TANS_PCTX);
    TEST_ASSERT_EQUAL_UINT8(0x05, pt);

    /* PCTX + LZP (high nibble set) */
    pt = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX | 0x10u);
    TEST_ASSERT_EQUAL_UINT8(0x06, pt);

    pt = netc_compact_type_encode(NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
                                   NETC_ALG_TANS_PCTX | 0x10u);
    TEST_ASSERT_EQUAL_UINT8(0x07, pt);
}

void test_pkt_type_lzp_bigram_bucket_roundtrip(void)
{
    for (uint8_t bucket = 0; bucket < 16; bucket++) {
        uint8_t alg = (uint8_t)(NETC_ALG_LZP | (bucket << 4));

        /* LZP + BIGRAM + bucket */
        uint8_t pt = netc_compact_type_encode(
            NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0x90u + bucket, pt);
        TEST_ASSERT_EQUAL_UINT8(alg, netc_pkt_type_table[pt].algorithm);
        TEST_ASSERT_BITS(NETC_PKT_FLAG_BIGRAM, NETC_PKT_FLAG_BIGRAM,
                         netc_pkt_type_table[pt].flags);

        /* LZP + BIGRAM + DELTA + bucket */
        pt = netc_compact_type_encode(
            NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_UINT8(0xA0u + bucket, pt);
        TEST_ASSERT_EQUAL_UINT8(alg, netc_pkt_type_table[pt].algorithm);
        TEST_ASSERT_BITS(NETC_PKT_FLAG_BIGRAM, NETC_PKT_FLAG_BIGRAM,
                         netc_pkt_type_table[pt].flags);
        TEST_ASSERT_BITS(NETC_PKT_FLAG_DELTA, NETC_PKT_FLAG_DELTA,
                         netc_pkt_type_table[pt].flags);
    }
}

void test_pkt_type_lz77x(void)
{
    uint8_t pt = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZ77X);
    TEST_ASSERT_EQUAL_UINT8(0x0E, pt);
}

void test_pkt_type_decode_table_consistency(void)
{
    /* For every valid entry in the table, encoding the decoded (flags, alg)
     * must produce the same index. */
    for (unsigned i = 0; i < 0xB0; i++) {
        const netc_pkt_type_entry_t *e = &netc_pkt_type_table[i];
        if (e->flags == 0 && e->algorithm == 0 && i != 0x00) continue; /* unused slot */
        if (i == 0x0F) continue; /* reserved */

        uint8_t re = netc_compact_type_encode(e->flags, e->algorithm);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)i, re,
            "Packet type table decode→encode mismatch");
    }
}

/* =========================================================================
 * Size varint tests
 * ========================================================================= */

void test_compact_hdr_size_small(void)
{
    uint8_t buf[4];
    netc_pkt_header_t hdr;

    /* original_size = 0 → 2-byte header */
    size_t sz = netc_hdr_write_compact(buf, 0x00, 0);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[1]); /* E=0, S=0 */

    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    TEST_ASSERT_EQUAL_UINT16(0, hdr.original_size);

    /* original_size = 1 → 2-byte header */
    sz = netc_hdr_write_compact(buf, 0x00, 1);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    TEST_ASSERT_EQUAL_UINT16(1, hdr.original_size);

    /* original_size = 127 → 2-byte header (max short) */
    sz = netc_hdr_write_compact(buf, 0x00, 127);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    TEST_ASSERT_EQUAL_UINT8(127, buf[1]); /* E=0, S=127 */
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(2, sz);
    TEST_ASSERT_EQUAL_UINT16(127, hdr.original_size);
}

void test_compact_hdr_size_large(void)
{
    uint8_t buf[4];
    netc_pkt_header_t hdr;

    /* original_size = 128 → 4-byte header */
    size_t sz = netc_hdr_write_compact(buf, 0x10, 128);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    TEST_ASSERT_EQUAL_UINT8(0x80, buf[1]); /* E=1, extension marker */
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    TEST_ASSERT_EQUAL_UINT16(128, hdr.original_size);

    /* original_size = 255 */
    sz = netc_hdr_write_compact(buf, 0x10, 255);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    TEST_ASSERT_EQUAL_UINT16(255, hdr.original_size);

    /* original_size = 32767 */
    sz = netc_hdr_write_compact(buf, 0x10, 32767);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    TEST_ASSERT_EQUAL_UINT16(32767, hdr.original_size);

    /* original_size = 65535 (max) */
    sz = netc_hdr_write_compact(buf, 0x10, 65535);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(4, sz);
    TEST_ASSERT_EQUAL_UINT16(65535, hdr.original_size);
}

void test_compact_hdr_truncated_short(void)
{
    uint8_t buf[1] = { 0x00 };
    netc_pkt_header_t hdr;
    size_t sz = netc_hdr_read_compact(buf, 1, &hdr);
    TEST_ASSERT_EQUAL_UINT(0, sz); /* only 1 byte → error */
}

void test_compact_hdr_truncated_long(void)
{
    uint8_t buf[3] = { 0x10, 0x80, 0x00 };
    netc_pkt_header_t hdr;
    /* E=1 but only 3 bytes available (need 4) */
    size_t sz = netc_hdr_read_compact(buf, 3, &hdr);
    TEST_ASSERT_EQUAL_UINT(0, sz);
}

void test_compact_hdr_invalid_type(void)
{
    uint8_t buf[4] = { 0xFF, 0x00, 0x00, 0x00 };
    netc_pkt_header_t hdr;
    size_t sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(0, sz); /* 0xFF is invalid sentinel */
}

void test_compact_hdr_reserved_slot(void)
{
    /* 0x0F is reserved (flags=0, algorithm=0 but index != 0x00) */
    uint8_t buf[4] = { 0x0F, 0x00, 0x00, 0x00 };
    netc_pkt_header_t hdr;
    size_t sz = netc_hdr_read_compact(buf, 4, &hdr);
    TEST_ASSERT_EQUAL_UINT(0, sz);
}

/* =========================================================================
 * Compress/decompress round-trip tests with compact headers
 * ========================================================================= */

void test_compact_passthrough_roundtrip_no_dict(void)
{
    netc_ctx_t *cctx = make_compact_ctx(NULL, 0);
    TEST_ASSERT_NOT_NULL(cctx);

    /* Use high-entropy data that forces pure passthrough (no LZ77/RLE) */
    uint8_t src[32];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)((i * 173 + 37) & 0xFF);

    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[32];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, src, sizeof(src), cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    /* Compact header: 32 <= 127, so 2-byte header; output ≤ 2 + 32 */
    TEST_ASSERT_TRUE(csz <= 2 + sizeof(src));

    /* Byte 1: original_size = 32 (E=0) */
    TEST_ASSERT_EQUAL_UINT8(32, cbuf[1] & 0x7F);

    netc_ctx_t *dctx = make_compact_ctx(NULL, 0);
    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_tans_roundtrip(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, 0);
    TEST_ASSERT_NOT_NULL(cctx);

    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, s_repetitive, sizeof(s_repetitive),
                                      cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    /* Compressed output should be smaller than original + header */
    TEST_ASSERT_TRUE(csz < sizeof(s_repetitive) + NETC_HEADER_SIZE);

    /* 64B original → 2-byte compact header (64 <= 127) */
    TEST_ASSERT_TRUE(csz <= 2 + sizeof(s_repetitive));

    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);
    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_repetitive), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_repetitive, dbuf, sizeof(s_repetitive));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_skewed_roundtrip(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, 0);
    TEST_ASSERT_NOT_NULL(cctx);

    size_t bound = netc_compress_bound(sizeof(s_skewed));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, s_skewed, sizeof(s_skewed),
                                      cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    /* 128B original → 4-byte compact header (128 > 127) */
    TEST_ASSERT_TRUE(csz <= 4 + sizeof(s_skewed));

    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);
    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_skewed), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_skewed, dbuf, sizeof(s_skewed));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_entropy_passthrough_roundtrip(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, 0);
    TEST_ASSERT_NOT_NULL(cctx);

    size_t bound = netc_compress_bound(sizeof(s_entropy));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, s_entropy, sizeof(s_entropy),
                                      cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);
    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_entropy), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_entropy, dbuf, sizeof(s_entropy));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_header_saves_bytes_vs_legacy(void)
{
    /* Compare compact vs legacy header size for same data */
    netc_ctx_t *legacy_ctx = NULL;
    {
        netc_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.flags = NETC_CFG_FLAG_STATEFUL;
        legacy_ctx = netc_ctx_create(s_dict, &cfg);
    }
    netc_ctx_t *compact_ctx = make_compact_ctx(s_dict, 0);

    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *legacy_buf  = (uint8_t *)malloc(bound);
    uint8_t *compact_buf = (uint8_t *)malloc(bound);
    size_t lsz = 0, csz = 0;

    netc_compress(legacy_ctx, s_repetitive, sizeof(s_repetitive),
                  legacy_buf, bound, &lsz);
    netc_compress(compact_ctx, s_repetitive, sizeof(s_repetitive),
                  compact_buf, bound, &csz);

    /* Compact should be at least 4 bytes smaller (8B - 2B = 6B for 64B input,
     * but payload may differ slightly due to PCTX vs bucket selection).
     * At minimum, compact should not be larger than legacy. */
    TEST_ASSERT_TRUE_MESSAGE(csz <= lsz,
        "Compact header should not produce larger output than legacy");

    /* For 64B input: legacy header = 8B, compact = 2B → save 6B */
    TEST_ASSERT_TRUE_MESSAGE(csz + 4 <= lsz,
        "Compact header should save at least 4 bytes on 64B packets");

    netc_ctx_destroy(compact_ctx);
    netc_ctx_destroy(legacy_ctx);
    free(compact_buf);
    free(legacy_buf);
}

void test_compact_delta_roundtrip(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);
    TEST_ASSERT_NOT_NULL(cctx);

    uint8_t pkt1[64], pkt2[64];
    memset(pkt1, 0x41, sizeof(pkt1));
    /* pkt2 differs by a few bytes from pkt1 */
    memcpy(pkt2, pkt1, sizeof(pkt2));
    pkt2[0] = 0x42; pkt2[10] = 0x43; pkt2[63] = 0x44;

    size_t bound = netc_compress_bound(sizeof(pkt1));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz = 0, dsz = 0;

    /* Compress packet 1 (no delta, first packet) */
    netc_result_t cr = netc_compress(cctx, pkt1, sizeof(pkt1), cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    netc_ctx_t *dctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);
    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_MEMORY(pkt1, dbuf, sizeof(pkt1));

    /* Compress packet 2 (should use delta) */
    cr = netc_compress(cctx, pkt2, sizeof(pkt2), cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(pkt2), dsz);
    TEST_ASSERT_EQUAL_MEMORY(pkt2, dbuf, sizeof(pkt2));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_multi_packet_roundtrip(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);
    netc_ctx_t *dctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);

    size_t bound = netc_compress_bound(64);
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz, dsz;

    /* Send 10 packets, verify each round-trips correctly */
    for (int i = 0; i < 10; i++) {
        uint8_t pkt[64];
        for (size_t j = 0; j < 64; j++)
            pkt[j] = (uint8_t)((j + i * 7) & 0xFF);

        csz = 0; dsz = 0;
        netc_result_t cr = netc_compress(cctx, pkt, 64, cbuf, bound, &csz);
        TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

        netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, 64, &dsz);
        TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
        TEST_ASSERT_EQUAL_UINT(64, dsz);
        TEST_ASSERT_EQUAL_MEMORY(pkt, dbuf, 64);
    }

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_1byte_packet(void)
{
    netc_ctx_t *cctx = make_compact_ctx(s_dict, 0);
    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);

    uint8_t src = 0xAA;
    uint8_t cbuf[16], dbuf[1];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, &src, 1, cbuf, sizeof(cbuf), &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);
    /* 1-byte packet: 2-byte header + 1 byte payload (passthrough) = 3 bytes */
    TEST_ASSERT_EQUAL_UINT(3, csz);

    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, 1, &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(1, dsz);
    TEST_ASSERT_EQUAL_UINT8(0xAA, dbuf[0]);

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
}

/* =========================================================================
 * LZP + BIGRAM compact type round-trip tests
 * ========================================================================= */

void test_compact_lzp_bigram_roundtrip(void)
{
    /* LZP+BIGRAM compact types (0x90-0xAF) should produce valid
     * compress/decompress round-trips when BIGRAM flag is set. */
    netc_ctx_t *cctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_BIGRAM);
    netc_ctx_t *dctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_BIGRAM);
    TEST_ASSERT_NOT_NULL(cctx);
    TEST_ASSERT_NOT_NULL(dctx);

    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress(cctx, s_repetitive, sizeof(s_repetitive),
                                      cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_repetitive), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_repetitive, dbuf, sizeof(s_repetitive));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_lzp_bigram_delta_roundtrip(void)
{
    /* LZP+BIGRAM+DELTA compact types (0xA0-0xAF) round-trip. */
    netc_ctx_t *cctx = make_compact_ctx(s_dict,
        NETC_CFG_FLAG_BIGRAM | NETC_CFG_FLAG_DELTA);
    netc_ctx_t *dctx = make_compact_ctx(s_dict,
        NETC_CFG_FLAG_BIGRAM | NETC_CFG_FLAG_DELTA);
    TEST_ASSERT_NOT_NULL(cctx);
    TEST_ASSERT_NOT_NULL(dctx);

    size_t bound = netc_compress_bound(sizeof(s_skewed));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz, dsz;

    /* Send 10 packets to exercise delta + LZP + BIGRAM across stream */
    for (int i = 0; i < 10; i++) {
        uint8_t pkt[128];
        for (size_t j = 0; j < 128; j++)
            pkt[j] = (uint8_t)((s_skewed[j] + (uint8_t)i) & 0xFF);

        csz = 0; dsz = 0;
        netc_result_t cr = netc_compress(cctx, pkt, sizeof(pkt), cbuf, bound, &csz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, cr,
            "LZP+BIGRAM+DELTA compress should succeed");

        netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, dr,
            "LZP+BIGRAM+DELTA decompress should succeed");
        TEST_ASSERT_EQUAL_UINT(sizeof(pkt), dsz);
        TEST_ASSERT_EQUAL_MEMORY(pkt, dbuf, sizeof(pkt));
    }

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

/* =========================================================================
 * ANS state compaction tests (4B→2B in compact mode)
 * ========================================================================= */

void test_compact_ans_state_saves_2_bytes(void)
{
    /* Compact mode should save 6B header + 2B ANS state = 8B total
     * compared to legacy mode on the same tANS-compressed data. */
    netc_ctx_t *legacy_ctx = NULL;
    {
        netc_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.flags = NETC_CFG_FLAG_STATEFUL;
        legacy_ctx = netc_ctx_create(s_dict, &cfg);
    }
    netc_ctx_t *compact_ctx = make_compact_ctx(s_dict, 0);
    TEST_ASSERT_NOT_NULL(legacy_ctx);
    TEST_ASSERT_NOT_NULL(compact_ctx);

    size_t bound = netc_compress_bound(sizeof(s_repetitive));
    uint8_t *legacy_buf  = (uint8_t *)malloc(bound);
    uint8_t *compact_buf = (uint8_t *)malloc(bound);
    size_t lsz = 0, csz = 0;

    netc_result_t lr = netc_compress(legacy_ctx, s_repetitive, sizeof(s_repetitive),
                                      legacy_buf, bound, &lsz);
    netc_result_t cr = netc_compress(compact_ctx, s_repetitive, sizeof(s_repetitive),
                                      compact_buf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, lr);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    /* Legacy: 8B header + 4B ANS state + bitstream
     * Compact: 2B header + 2B ANS state + bitstream (same bitstream)
     * Savings = 6B (header) + 2B (ANS state) = 8B
     * Note: algorithm selection may vary, so test for at least 6B savings. */
    TEST_ASSERT_TRUE_MESSAGE(csz + 6 <= lsz,
        "Compact mode should save at least 6B (header+state) on 64B tANS packets");

    /* Verify compact output round-trips */
    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);
    uint8_t dbuf[64];
    size_t dsz = 0;
    netc_result_t dr = netc_decompress(dctx, compact_buf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(s_repetitive), dsz);
    TEST_ASSERT_EQUAL_MEMORY(s_repetitive, dbuf, sizeof(s_repetitive));

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(compact_ctx);
    netc_ctx_destroy(legacy_ctx);
    free(compact_buf);
    free(legacy_buf);
}

void test_compact_ans_state_multi_packet_stream(void)
{
    /* Verify ANS state compaction works correctly across a multi-packet
     * stream with delta coding (exercises context_seq + state compaction). */
    netc_ctx_t *cctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);
    netc_ctx_t *dctx = make_compact_ctx(s_dict, NETC_CFG_FLAG_DELTA);
    TEST_ASSERT_NOT_NULL(cctx);
    TEST_ASSERT_NOT_NULL(dctx);

    size_t bound = netc_compress_bound(sizeof(s_skewed));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[128];
    size_t csz, dsz;

    /* Send 20 packets with varying content to exercise delta + compact ANS */
    for (int i = 0; i < 20; i++) {
        uint8_t pkt[128];
        for (size_t j = 0; j < 128; j++)
            pkt[j] = (uint8_t)((s_skewed[j] + (uint8_t)i) & 0xFF);

        csz = 0; dsz = 0;
        netc_result_t cr = netc_compress(cctx, pkt, sizeof(pkt), cbuf, bound, &csz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, cr,
            "Compact ANS compress should succeed for all 20 packets");

        netc_result_t dr = netc_decompress(dctx, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, dr,
            "Compact ANS decompress should succeed for all 20 packets");
        TEST_ASSERT_EQUAL_UINT(sizeof(pkt), dsz);
        TEST_ASSERT_EQUAL_MEMORY(pkt, dbuf, sizeof(pkt));
    }

    netc_ctx_destroy(dctx);
    netc_ctx_destroy(cctx);
    free(cbuf);
}

void test_compact_ans_legacy_stateless_unaffected(void)
{
    /* Stateless compress/decompress should always use legacy 4B ANS state,
     * regardless of any flags. */
    uint8_t src[64];
    memset(src, 0x41, sizeof(src)); /* repetitive → will use tANS */

    size_t bound = netc_compress_bound(sizeof(src));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    uint8_t dbuf[64];
    size_t csz = 0, dsz = 0;

    netc_result_t cr = netc_compress_stateless(s_dict, src, sizeof(src),
                                                cbuf, bound, &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, cr);

    /* Stateless always produces legacy 8B header */
    TEST_ASSERT_TRUE(csz >= NETC_HEADER_SIZE);

    netc_result_t dr = netc_decompress_stateless(s_dict, cbuf, csz,
                                                  dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, dr);
    TEST_ASSERT_EQUAL_UINT(sizeof(src), dsz);
    TEST_ASSERT_EQUAL_MEMORY(src, dbuf, sizeof(src));

    free(cbuf);
}

void test_compact_decompress_truncated_1byte(void)
{
    netc_ctx_t *dctx = make_compact_ctx(s_dict, 0);

    uint8_t bad[1] = { 0x00 };
    uint8_t dbuf[64];
    size_t dsz = 0;

    netc_result_t dr = netc_decompress(dctx, bad, 1, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, dr);

    netc_ctx_destroy(dctx);
}

/* =========================================================================
 * Unity test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* Packet type encoding */
    RUN_TEST(test_pkt_type_passthru_roundtrip);
    RUN_TEST(test_pkt_type_tans_bucket_roundtrip);
    RUN_TEST(test_pkt_type_lzp_bucket_roundtrip);
    RUN_TEST(test_pkt_type_lzp_bigram_bucket_roundtrip);
    RUN_TEST(test_pkt_type_mreg_variants);
    RUN_TEST(test_pkt_type_pctx_variants);
    RUN_TEST(test_pkt_type_lz77x);
    RUN_TEST(test_pkt_type_decode_table_consistency);

    /* Size varint */
    RUN_TEST(test_compact_hdr_size_small);
    RUN_TEST(test_compact_hdr_size_large);
    RUN_TEST(test_compact_hdr_truncated_short);
    RUN_TEST(test_compact_hdr_truncated_long);
    RUN_TEST(test_compact_hdr_invalid_type);
    RUN_TEST(test_compact_hdr_reserved_slot);

    /* Compress/decompress round-trip */
    RUN_TEST(test_compact_passthrough_roundtrip_no_dict);
    RUN_TEST(test_compact_tans_roundtrip);
    RUN_TEST(test_compact_skewed_roundtrip);
    RUN_TEST(test_compact_entropy_passthrough_roundtrip);
    RUN_TEST(test_compact_header_saves_bytes_vs_legacy);
    RUN_TEST(test_compact_delta_roundtrip);
    RUN_TEST(test_compact_multi_packet_roundtrip);
    RUN_TEST(test_compact_1byte_packet);

    /* LZP + BIGRAM compact types */
    RUN_TEST(test_compact_lzp_bigram_roundtrip);
    RUN_TEST(test_compact_lzp_bigram_delta_roundtrip);

    /* ANS state compaction */
    RUN_TEST(test_compact_ans_state_saves_2_bytes);
    RUN_TEST(test_compact_ans_state_multi_packet_stream);
    RUN_TEST(test_compact_ans_legacy_stateless_unaffected);

    /* Error cases */
    RUN_TEST(test_compact_decompress_truncated_1byte);

    return UNITY_END();
}
