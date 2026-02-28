/**
 * test_tans_10bit.c -- Comprehensive tests for 10-bit tANS support.
 *
 * Tests cover:
 *   1. Frequency rescaling (12-bit -> 10-bit)
 *   2. 10-bit table build
 *   3. 10-bit encode/decode round-trip
 *   4. End-to-end compress/decompress with 10-bit competition
 *   5. Compact header packet type encoding/decoding for TANS_10
 *   6. Edge cases and error paths
 */

#include "unity.h"
#include "../include/netc.h"
#include "../src/algo/netc_tans.h"
#include "../src/util/netc_bitstream.h"
#include "../src/core/netc_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Helper: build a 12-bit freq table with given symbol counts
 * ========================================================================= */
static void make_freq_12(netc_freq_table_t *ft, int n_syms, const uint16_t *freqs) {
    memset(ft, 0, sizeof(*ft));
    for (int i = 0; i < n_syms && i < 256; i++) {
        ft->freq[i] = freqs[i];
    }
}

/* =========================================================================
 * 1. Frequency rescaling tests
 * ========================================================================= */

void test_rescale_uniform_2sym(void) {
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0x41] = 2048;
    ft12.freq[0x42] = 2048;

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* Verify sum = 1024 */
    uint32_t total = 0;
    for (int s = 0; s < 256; s++) total += ft10.freq[s];
    TEST_ASSERT_EQUAL_UINT32(NETC_TANS_TABLE_SIZE_10, total);

    /* Uniform 2-symbol: each should be 512 */
    TEST_ASSERT_EQUAL_UINT16(512, ft10.freq[0x41]);
    TEST_ASSERT_EQUAL_UINT16(512, ft10.freq[0x42]);
}

void test_rescale_skewed_2sym(void) {
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0x41] = 3072;
    ft12.freq[0x42] = 1024;

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    uint32_t total = 0;
    for (int s = 0; s < 256; s++) total += ft10.freq[s];
    TEST_ASSERT_EQUAL_UINT32(NETC_TANS_TABLE_SIZE_10, total);

    /* 3072/4096 * 1024 = 768, 1024/4096 * 1024 = 256 */
    TEST_ASSERT_EQUAL_UINT16(768, ft10.freq[0x41]);
    TEST_ASSERT_EQUAL_UINT16(256, ft10.freq[0x42]);
}

void test_rescale_many_symbols(void) {
    /* Create a realistic distribution: one dominant + many rare */
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0] = 3000;
    /* Spread remaining 1096 across 137 symbols (each ~8) */
    uint32_t rem = 4096 - 3000;
    for (int s = 1; s <= 137 && rem > 0; s++) {
        uint16_t f = (rem > 8) ? 8 : (uint16_t)rem;
        ft12.freq[s] = f;
        rem -= f;
    }
    /* Fix any leftover */
    if (rem > 0) ft12.freq[1] += (uint16_t)rem;

    /* Verify 12-bit table sums correctly */
    uint32_t tot12 = 0;
    for (int s = 0; s < 256; s++) tot12 += ft12.freq[s];
    TEST_ASSERT_EQUAL_UINT32(4096, tot12);

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    uint32_t total = 0;
    int nonzero_count = 0;
    for (int s = 0; s < 256; s++) {
        total += ft10.freq[s];
        if (ft12.freq[s] > 0) {
            /* Non-zero symbols must have freq >= 1 */
            TEST_ASSERT_GREATER_OR_EQUAL_UINT16(1, ft10.freq[s]);
            nonzero_count++;
        } else {
            TEST_ASSERT_EQUAL_UINT16(0, ft10.freq[s]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(NETC_TANS_TABLE_SIZE_10, total);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, nonzero_count);
}

void test_rescale_single_symbol(void) {
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0xFF] = 4096;

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    uint32_t total = 0;
    for (int s = 0; s < 256; s++) total += ft10.freq[s];
    TEST_ASSERT_EQUAL_UINT32(NETC_TANS_TABLE_SIZE_10, total);
    TEST_ASSERT_EQUAL_UINT16(1024, ft10.freq[0xFF]);
}

void test_rescale_null_args(void) {
    netc_freq_table_t ft;
    TEST_ASSERT_EQUAL_INT(-1, netc_freq_rescale_12_to_10(NULL, &ft));
    TEST_ASSERT_EQUAL_INT(-1, netc_freq_rescale_12_to_10(&ft, NULL));
    TEST_ASSERT_EQUAL_INT(-1, netc_freq_rescale_12_to_10(NULL, NULL));
}

void test_rescale_bad_sum(void) {
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0] = 1000; /* Sum != 4096 */

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

void test_rescale_minimum_frequency_preservation(void) {
    /* Test that symbols with freq=1 in 12-bit still get freq>=1 in 10-bit */
    netc_freq_table_t ft12, ft10;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0] = 4096 - 10; /* dominant */
    for (int s = 1; s <= 10; s++) ft12.freq[s] = 1; /* 10 rare symbols */

    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    uint32_t total = 0;
    for (int s = 0; s < 256; s++) {
        total += ft10.freq[s];
        if (ft12.freq[s] > 0) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT16(1, ft10.freq[s]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(NETC_TANS_TABLE_SIZE_10, total);
}

/* =========================================================================
 * 2. 10-bit table build tests
 * ========================================================================= */

void test_build_10_uniform_2sym(void) {
    netc_freq_table_t ft10;
    memset(&ft10, 0, sizeof(ft10));
    ft10.freq[0x41] = 512;
    ft10.freq[0x42] = 512;

    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT8(1, tbl.valid);
}

void test_build_10_single_symbol(void) {
    netc_freq_table_t ft10;
    memset(&ft10, 0, sizeof(ft10));
    ft10.freq[0x00] = 1024;

    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT8(1, tbl.valid);

    /* All decode entries should map to symbol 0 */
    for (int i = 0; i < (int)NETC_TANS_TABLE_SIZE_10; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x00, tbl.decode[i].symbol);
    }
}

void test_build_10_null_args(void) {
    netc_freq_table_t ft;
    netc_tans_table_10_t tbl;
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_build_10(NULL, &ft));
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_build_10(&tbl, NULL));
}

void test_build_10_bad_sum(void) {
    netc_freq_table_t ft10;
    memset(&ft10, 0, sizeof(ft10));
    ft10.freq[0] = 500; /* Sum != 1024 */

    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, &ft10);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

void test_build_10_many_symbols(void) {
    /* Realistic distribution: 20 symbols */
    netc_freq_table_t ft10;
    memset(&ft10, 0, sizeof(ft10));
    ft10.freq[0x00] = 800;
    uint32_t rem = 1024 - 800;
    for (int s = 1; s <= 19 && rem > 0; s++) {
        uint16_t f = (rem > 12) ? 12 : (uint16_t)rem;
        ft10.freq[s] = f;
        rem -= f;
    }
    if (rem > 0) ft10.freq[1] += (uint16_t)rem;

    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT8(1, tbl.valid);
}

/* =========================================================================
 * 3. 10-bit encode/decode round-trip tests
 * ========================================================================= */

static void do_10bit_roundtrip(const char *name,
    const netc_freq_table_t *freq10,
    const uint8_t *src, size_t src_size)
{
    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, freq10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r, name);

    uint8_t buf[65536];
    netc_bsw_t bsw;
    netc_bsw_init(&bsw, buf, sizeof(buf));

    uint32_t fs = netc_tans_encode_10(&tbl, src, src_size,
                                       &bsw, NETC_TANS_TABLE_SIZE_10);
    size_t bs = netc_bsw_flush(&bsw);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, fs, name);
    /* State must be in [1024, 2048) */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(1024, fs, name);
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(2048, fs, name);
    TEST_ASSERT_NOT_EQUAL_MESSAGE((size_t)-1, bs, name);

    uint8_t dst[65536];
    memset(dst, 0xCC, sizeof(dst));
    netc_bsr_t bsr;
    netc_bsr_init(&bsr, buf, bs);

    int dr = netc_tans_decode_10(&tbl, &bsr, dst, src_size, fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dr, name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(src, dst, src_size, name);
}

void test_10bit_roundtrip_uniform_2sym(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x41] = 512;
    ft.freq[0x42] = 512;

    uint8_t src[8] = {0x41, 0x41, 0x42, 0x41, 0x41, 0x42, 0x41, 0x41};
    do_10bit_roundtrip("10bit_2sym_512_512", &ft, src, 8);
}

void test_10bit_roundtrip_skewed(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x41] = 768;
    ft.freq[0x42] = 256;

    uint8_t src[8] = {0x41, 0x41, 0x41, 0x42, 0x41, 0x41, 0x41, 0x41};
    do_10bit_roundtrip("10bit_2sym_768_256", &ft, src, 8);
}

void test_10bit_roundtrip_single_symbol(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x00] = 1024;

    uint8_t src[32];
    memset(src, 0x00, 32);
    do_10bit_roundtrip("10bit_single_sym", &ft, src, 32);
}

void test_10bit_roundtrip_128B_packet(void) {
    /* Simulate a 128-byte packet with realistic distribution */
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x00] = 600;
    ft.freq[0x01] = 200;
    ft.freq[0x02] = 100;
    ft.freq[0x03] = 50;
    ft.freq[0x04] = 30;
    ft.freq[0x05] = 20;
    ft.freq[0x06] = 10;
    ft.freq[0x07] = 10;
    ft.freq[0x08] = 4;

    uint8_t src[128];
    /* Fill with symbols matching distribution roughly */
    memset(src, 0x00, 75);
    memset(src + 75, 0x01, 25);
    memset(src + 100, 0x02, 12);
    memset(src + 112, 0x03, 6);
    memset(src + 118, 0x04, 4);
    memset(src + 122, 0x05, 3);
    memset(src + 125, 0x06, 1);
    memset(src + 126, 0x07, 1);
    memset(src + 127, 0x08, 1);

    do_10bit_roundtrip("10bit_128B", &ft, src, 128);
}

void test_10bit_roundtrip_64B_packet(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x00] = 800;
    ft.freq[0xFF] = 200;
    ft.freq[0x55] = 24;

    uint8_t src[64];
    memset(src, 0x00, 50);
    memset(src + 50, 0xFF, 12);
    memset(src + 62, 0x55, 2);

    do_10bit_roundtrip("10bit_64B", &ft, src, 64);
}

void test_10bit_roundtrip_1_byte(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0xAB] = 1024;

    uint8_t src[1] = {0xAB};
    do_10bit_roundtrip("10bit_1byte", &ft, src, 1);
}

/* =========================================================================
 * 4. 10-bit encode/decode error path tests
 * ========================================================================= */

void test_10bit_encode_null_args(void) {
    netc_tans_table_10_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    tbl.valid = 1;
    uint8_t src[4] = {0};
    netc_bsw_t bsw;
    uint8_t buf[64];
    netc_bsw_init(&bsw, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT32(0, netc_tans_encode_10(NULL, src, 4, &bsw, 1024));
    TEST_ASSERT_EQUAL_UINT32(0, netc_tans_encode_10(&tbl, NULL, 4, &bsw, 1024));
    TEST_ASSERT_EQUAL_UINT32(0, netc_tans_encode_10(&tbl, src, 4, NULL, 1024));
    TEST_ASSERT_EQUAL_UINT32(0, netc_tans_encode_10(&tbl, src, 0, &bsw, 1024));
}

void test_10bit_encode_invalid_table(void) {
    netc_tans_table_10_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    tbl.valid = 0;  /* not built */
    uint8_t src[4] = {0};
    netc_bsw_t bsw;
    uint8_t buf[64];
    netc_bsw_init(&bsw, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT32(0, netc_tans_encode_10(&tbl, src, 4, &bsw, 1024));
}

void test_10bit_encode_symbol_not_in_table(void) {
    /* Build table with only symbol 0, try to encode symbol 1 */
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0] = 1024;

    netc_tans_table_10_t tbl;
    netc_tans_build_10(&tbl, &ft);

    uint8_t src[4] = {0x01, 0x01, 0x01, 0x01};  /* symbol 1 not in table */
    netc_bsw_t bsw;
    uint8_t buf[64];
    netc_bsw_init(&bsw, buf, sizeof(buf));

    uint32_t fs = netc_tans_encode_10(&tbl, src, 4, &bsw, NETC_TANS_TABLE_SIZE_10);
    TEST_ASSERT_EQUAL_UINT32(0, fs);  /* should fail */
}

void test_10bit_decode_null_args(void) {
    netc_tans_table_10_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    tbl.valid = 1;
    uint8_t dst[4];
    netc_bsr_t bsr;
    uint8_t buf[4] = {0xFF};
    netc_bsr_init(&bsr, buf, 1);

    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(NULL, &bsr, dst, 4, 1024));
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(&tbl, NULL, dst, 4, 1024));
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(&tbl, &bsr, NULL, 4, 1024));
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(&tbl, &bsr, dst, 0, 1024));
}

void test_10bit_decode_invalid_state(void) {
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0] = 1024;

    netc_tans_table_10_t tbl;
    netc_tans_build_10(&tbl, &ft);

    uint8_t dst[4];
    netc_bsr_t bsr;
    uint8_t buf[4] = {0xFF};
    netc_bsr_init(&bsr, buf, 1);

    /* State below range [1024, 2048) */
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(&tbl, &bsr, dst, 4, 512));
    /* State above range */
    TEST_ASSERT_EQUAL_INT(-1, netc_tans_decode_10(&tbl, &bsr, dst, 4, 2048));
    /* State at exact boundary (should be valid) */
    netc_bsr_init(&bsr, buf, 1);
    TEST_ASSERT_EQUAL_INT(0, netc_tans_decode_10(&tbl, &bsr, dst, 1, 1024));
}

/* =========================================================================
 * 5. Full rescale + build + roundtrip pipeline
 * ========================================================================= */

void test_rescale_build_roundtrip_2sym(void) {
    /* Start with 12-bit table, rescale to 10-bit, encode, decode */
    netc_freq_table_t ft12;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0x41] = 3072;
    ft12.freq[0x42] = 1024;

    netc_freq_table_t ft10;
    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    uint8_t src[16] = {0x41,0x41,0x41,0x42,0x41,0x41,0x41,0x41,
                       0x41,0x42,0x41,0x41,0x41,0x41,0x41,0x41};
    do_10bit_roundtrip("rescale_build_roundtrip_2sym", &ft10, src, 16);
}

void test_rescale_build_roundtrip_many_sym(void) {
    /* Start with a 12-bit table with many symbols */
    netc_freq_table_t ft12;
    memset(&ft12, 0, sizeof(ft12));
    ft12.freq[0] = 3600;
    ft12.freq[1] = 200;
    ft12.freq[2] = 100;
    ft12.freq[3] = 50;
    ft12.freq[4] = 50;
    ft12.freq[5] = 30;
    ft12.freq[6] = 30;
    ft12.freq[7] = 20;
    ft12.freq[8] = 10;
    ft12.freq[9] = 6;

    netc_freq_table_t ft10;
    int r = netc_freq_rescale_12_to_10(&ft12, &ft10);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* Create source using only symbols in the table */
    uint8_t src[64];
    memset(src, 0x00, 50);
    memset(src + 50, 0x01, 8);
    memset(src + 58, 0x02, 3);
    memset(src + 61, 0x03, 1);
    memset(src + 62, 0x04, 1);
    memset(src + 63, 0x05, 1);

    do_10bit_roundtrip("rescale_build_roundtrip_many", &ft10, src, 64);
}

/* =========================================================================
 * 6. Compact packet type encoding/decoding for TANS_10
 * ========================================================================= */

void test_compact_type_encode_tans_10(void) {
    /* TANS_10 + bucket 0 */
    uint8_t ptype = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID,
                                              NETC_ALG_TANS_10 | (0u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xB0, ptype);

    /* TANS_10 + bucket 5 */
    ptype = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID,
                                     NETC_ALG_TANS_10 | (5u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xB5, ptype);

    /* TANS_10 + bucket 15 */
    ptype = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID,
                                     NETC_ALG_TANS_10 | (15u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xBF, ptype);
}

void test_compact_type_encode_tans_10_delta(void) {
    /* TANS_10 + DELTA + bucket 0 */
    uint8_t ptype = netc_compact_type_encode(
        NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_TANS_10 | (0u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xC0, ptype);

    /* TANS_10 + DELTA + bucket 7 */
    ptype = netc_compact_type_encode(
        NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_TANS_10 | (7u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xC7, ptype);

    /* TANS_10 + DELTA + bucket 15 */
    ptype = netc_compact_type_encode(
        NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
        NETC_ALG_TANS_10 | (15u << 4));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0xCF, ptype);
}

void test_compact_type_decode_tans_10(void) {
    /* Verify the decode table entries for 0xB0-0xBF */
    for (uint8_t b = 0; b < 16; b++) {
        const netc_pkt_type_entry_t *e = &netc_pkt_type_table[0xB0 + b];
        TEST_ASSERT_EQUAL_HEX8(NETC_PKT_FLAG_DICT_ID, e->flags);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(NETC_ALG_TANS_10 | ((uint8_t)b << 4)),
                               e->algorithm);
    }
    /* Verify 0xC0-0xCF (TANS_10 + DELTA) */
    for (uint8_t b = 0; b < 16; b++) {
        const netc_pkt_type_entry_t *e = &netc_pkt_type_table[0xC0 + b];
        TEST_ASSERT_EQUAL_HEX8(NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID,
                               e->flags);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(NETC_ALG_TANS_10 | ((uint8_t)b << 4)),
                               e->algorithm);
    }
}

void test_compact_type_roundtrip_tans_10(void) {
    /* Encode then decode via compact header */
    for (uint8_t b = 0; b < 16; b++) {
        uint8_t alg = (uint8_t)(NETC_ALG_TANS_10 | ((uint8_t)b << 4));
        uint8_t ptype = netc_compact_type_encode(NETC_PKT_FLAG_DICT_ID, alg);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0xB0 + b), ptype);

        /* Write compact header and read it back */
        uint8_t hdr_buf[4];
        size_t w = netc_hdr_write_compact(hdr_buf, ptype, 64);
        TEST_ASSERT_EQUAL_INT(2, (int)w); /* 64 <= 127 -> 2-byte header */

        netc_pkt_header_t hdr;
        size_t r = netc_hdr_read_compact(hdr_buf, w, &hdr);
        TEST_ASSERT_EQUAL_INT(2, (int)r);
        TEST_ASSERT_EQUAL_UINT16(64, hdr.original_size);
        TEST_ASSERT_EQUAL_HEX8(NETC_PKT_FLAG_DICT_ID, hdr.flags);
        TEST_ASSERT_EQUAL_HEX8(alg, hdr.algorithm);
    }
}

/* =========================================================================
 * 7. End-to-end compress/decompress with 10-bit competition
 * ========================================================================= */

void test_e2e_small_packet_compact_mode(void) {
    /* Train a dictionary with small packets, then compress/decompress
     * a 64-byte packet in compact mode. The compressor may choose 10-bit
     * if it produces smaller output. Either way, decompress must succeed. */

    /* Create training corpus: 100 similar 64-byte packets */
    uint8_t corpus[100][64];
    const uint8_t *ptrs[100];
    size_t sizes[100];
    for (int i = 0; i < 100; i++) {
        memset(corpus[i], 0x00, 64);
        corpus[i][0] = 0x01;
        corpus[i][1] = (uint8_t)i;
        corpus[i][2] = 0x02;
        corpus[i][3] = (uint8_t)(i & 0x0F);
        ptrs[i] = corpus[i];
        sizes[i] = 64;
    }

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(ptrs, sizes, 100, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(dict);

    /* Create context with compact headers */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR;
    cfg.compression_level = 5;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    /* Compress and decompress a test packet */
    uint8_t test_pkt[64];
    memset(test_pkt, 0x00, 64);
    test_pkt[0] = 0x01;
    test_pkt[1] = 0x42;
    test_pkt[2] = 0x02;
    test_pkt[3] = 0x03;

    uint8_t compressed[128];
    size_t  comp_sz = 0;
    r = netc_compress(enc, test_pkt, 64, compressed, sizeof(compressed), &comp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_GREATER_THAN(0, comp_sz);
    TEST_ASSERT_LESS_OR_EQUAL(64 + NETC_MAX_OVERHEAD, comp_sz);

    uint8_t decompressed[128];
    size_t  decomp_sz = 0;
    r = netc_decompress(dec, compressed, comp_sz, decompressed, sizeof(decompressed), &decomp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_INT(64, (int)decomp_sz);
    TEST_ASSERT_EQUAL_MEMORY(test_pkt, decompressed, 64);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

void test_e2e_128B_packet_compact_mode(void) {
    /* Test boundary: 128 bytes is the max for 10-bit trial */
    uint8_t corpus[50][128];
    const uint8_t *ptrs[50];
    size_t sizes[50];
    for (int i = 0; i < 50; i++) {
        memset(corpus[i], 0x00, 128);
        corpus[i][0] = 0x01;
        corpus[i][1] = (uint8_t)i;
        for (int j = 64; j < 128; j++) corpus[i][j] = 0x55;
        ptrs[i] = corpus[i];
        sizes[i] = 128;
    }

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(ptrs, sizes, 50, 2, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t test_pkt[128];
    memset(test_pkt, 0x00, 64);
    memset(test_pkt + 64, 0x55, 64);
    test_pkt[0] = 0x01;
    test_pkt[1] = 0x99;

    uint8_t compressed[256];
    size_t  comp_sz = 0;
    r = netc_compress(enc, test_pkt, 128, compressed, sizeof(compressed), &comp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    uint8_t decompressed[256];
    size_t  decomp_sz = 0;
    r = netc_decompress(dec, compressed, comp_sz, decompressed, sizeof(decompressed), &decomp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_INT(128, (int)decomp_sz);
    TEST_ASSERT_EQUAL_MEMORY(test_pkt, decompressed, 128);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

void test_e2e_256B_packet_no_10bit(void) {
    /* 256-byte packet should NOT use 10-bit (threshold is <=128B).
     * Just verify compress/decompress still works correctly. */
    uint8_t corpus[30][256];
    const uint8_t *ptrs[30];
    size_t sizes[30];
    for (int i = 0; i < 30; i++) {
        memset(corpus[i], 0x00, 256);
        corpus[i][0] = (uint8_t)i;
        ptrs[i] = corpus[i];
        sizes[i] = 256;
    }

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(ptrs, sizes, 30, 3, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);

    uint8_t test_pkt[256];
    memset(test_pkt, 0x00, 256);
    test_pkt[0] = 0x42;

    uint8_t compressed[512];
    size_t comp_sz = 0;
    r = netc_compress(enc, test_pkt, 256, compressed, sizeof(compressed), &comp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    uint8_t decompressed[512];
    size_t decomp_sz = 0;
    r = netc_decompress(dec, compressed, comp_sz, decompressed, sizeof(decompressed), &decomp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_INT(256, (int)decomp_sz);
    TEST_ASSERT_EQUAL_MEMORY(test_pkt, decompressed, 256);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

void test_e2e_legacy_header_no_10bit(void) {
    /* Without compact headers, 10-bit should NOT be used.
     * Verify compress/decompress still works. */
    uint8_t corpus[50][64];
    const uint8_t *ptrs[50];
    size_t sizes[50];
    for (int i = 0; i < 50; i++) {
        memset(corpus[i], 0x00, 64);
        corpus[i][0] = (uint8_t)i;
        ptrs[i] = corpus[i];
        sizes[i] = 64;
    }

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(ptrs, sizes, 50, 4, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL; /* NO compact header */

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);

    uint8_t test_pkt[64];
    memset(test_pkt, 0x00, 64);
    test_pkt[0] = 0x42;

    uint8_t compressed[128];
    size_t comp_sz = 0;
    r = netc_compress(enc, test_pkt, 64, compressed, sizeof(compressed), &comp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    uint8_t decompressed[128];
    size_t decomp_sz = 0;
    r = netc_decompress(dec, compressed, comp_sz, decompressed, sizeof(decompressed), &decomp_sz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_INT(64, (int)decomp_sz);
    TEST_ASSERT_EQUAL_MEMORY(test_pkt, decompressed, 64);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

void test_e2e_multiple_packets_sequential(void) {
    /* Compress and decompress multiple small packets sequentially to verify
     * the 10-bit path doesn't corrupt stateful context (ring buffer, prev_pkt). */
    uint8_t corpus[50][80];
    const uint8_t *ptrs[50];
    size_t sizes[50];
    for (int i = 0; i < 50; i++) {
        memset(corpus[i], 0x00, 80);
        corpus[i][0] = 0x10;
        corpus[i][1] = (uint8_t)i;
        ptrs[i] = corpus[i];
        sizes[i] = 80;
    }

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(ptrs, sizes, 50, 5, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_COMPACT_HDR;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);

    for (int i = 0; i < 10; i++) {
        uint8_t pkt[80];
        memset(pkt, 0x00, 80);
        pkt[0] = 0x10;
        pkt[1] = (uint8_t)(i + 100);
        pkt[2] = (uint8_t)(i * 3);

        uint8_t comp[128];
        size_t comp_sz = 0;
        r = netc_compress(enc, pkt, 80, comp, sizeof(comp), &comp_sz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, r, "compress sequential");

        uint8_t decomp[128];
        size_t decomp_sz = 0;
        r = netc_decompress(dec, comp, comp_sz, decomp, sizeof(decomp), &decomp_sz);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, r, "decompress sequential");
        TEST_ASSERT_EQUAL_INT(80, (int)decomp_sz);
        TEST_ASSERT_EQUAL_MEMORY(pkt, decomp, 80);
    }

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}

/* =========================================================================
 * 8. State range validation tests
 * ========================================================================= */

void test_10bit_state_range(void) {
    /* Verify that after encoding, the final state is always in [1024, 2048) */
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x00] = 900;
    ft.freq[0x01] = 100;
    ft.freq[0x02] = 20;
    ft.freq[0x03] = 4;

    netc_tans_table_10_t tbl;
    int r = netc_tans_build_10(&tbl, &ft);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* Try multiple different inputs */
    for (int trial = 0; trial < 16; trial++) {
        uint8_t src[32];
        memset(src, 0x00, sizeof(src));
        src[0] = (uint8_t)(trial & 0x03);
        src[1] = 0x01;
        src[2] = 0x00;
        src[3] = (uint8_t)(trial >> 2);

        uint8_t buf[256];
        netc_bsw_t bsw;
        netc_bsw_init(&bsw, buf, sizeof(buf));

        uint32_t fs = netc_tans_encode_10(&tbl, src, 32, &bsw, NETC_TANS_TABLE_SIZE_10);
        if (fs != 0) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1024, fs);
            TEST_ASSERT_LESS_THAN_UINT32(2048, fs);
        }
    }
}

/* =========================================================================
 * 9. Spread step coprimality test
 * ========================================================================= */

void test_spread_step_coprime(void) {
    /* Verify GCD(643, 1024) = 1 by checking that stepping through
     * 1024 positions visits all positions exactly once */
    uint8_t visited[1024];
    memset(visited, 0, sizeof(visited));

    uint32_t pos = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, visited[pos], "position visited twice");
        visited[pos] = 1;
        pos = (pos + NETC_TANS_SPREAD_STEP_10) & (NETC_TANS_TABLE_SIZE_10 - 1U);
    }

    /* All positions should be visited */
    for (uint32_t i = 0; i < 1024; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, visited[i], "position not visited");
    }
}

/* =========================================================================
 * 10. Constants verification
 * ========================================================================= */

void test_constants(void) {
    TEST_ASSERT_EQUAL_UINT32(10, NETC_TANS_TABLE_LOG_10);
    TEST_ASSERT_EQUAL_UINT32(1024, NETC_TANS_TABLE_SIZE_10);
    TEST_ASSERT_EQUAL_UINT32(643, NETC_TANS_SPREAD_STEP_10);
    TEST_ASSERT_EQUAL_UINT32(0x06, NETC_ALG_TANS_10);
}

/* =========================================================================
 * Unity test runner
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* Frequency rescaling */
    RUN_TEST(test_rescale_uniform_2sym);
    RUN_TEST(test_rescale_skewed_2sym);
    RUN_TEST(test_rescale_many_symbols);
    RUN_TEST(test_rescale_single_symbol);
    RUN_TEST(test_rescale_null_args);
    RUN_TEST(test_rescale_bad_sum);
    RUN_TEST(test_rescale_minimum_frequency_preservation);

    /* Table build */
    RUN_TEST(test_build_10_uniform_2sym);
    RUN_TEST(test_build_10_single_symbol);
    RUN_TEST(test_build_10_null_args);
    RUN_TEST(test_build_10_bad_sum);
    RUN_TEST(test_build_10_many_symbols);

    /* Encode/decode round-trip */
    RUN_TEST(test_10bit_roundtrip_uniform_2sym);
    RUN_TEST(test_10bit_roundtrip_skewed);
    RUN_TEST(test_10bit_roundtrip_single_symbol);
    RUN_TEST(test_10bit_roundtrip_128B_packet);
    RUN_TEST(test_10bit_roundtrip_64B_packet);
    RUN_TEST(test_10bit_roundtrip_1_byte);

    /* Error paths */
    RUN_TEST(test_10bit_encode_null_args);
    RUN_TEST(test_10bit_encode_invalid_table);
    RUN_TEST(test_10bit_encode_symbol_not_in_table);
    RUN_TEST(test_10bit_decode_null_args);
    RUN_TEST(test_10bit_decode_invalid_state);

    /* Full pipeline */
    RUN_TEST(test_rescale_build_roundtrip_2sym);
    RUN_TEST(test_rescale_build_roundtrip_many_sym);

    /* Compact header encoding */
    RUN_TEST(test_compact_type_encode_tans_10);
    RUN_TEST(test_compact_type_encode_tans_10_delta);
    RUN_TEST(test_compact_type_decode_tans_10);
    RUN_TEST(test_compact_type_roundtrip_tans_10);

    /* End-to-end */
    RUN_TEST(test_e2e_small_packet_compact_mode);
    RUN_TEST(test_e2e_128B_packet_compact_mode);
    RUN_TEST(test_e2e_256B_packet_no_10bit);
    RUN_TEST(test_e2e_legacy_header_no_10bit);
    RUN_TEST(test_e2e_multiple_packets_sequential);

    /* State range */
    RUN_TEST(test_10bit_state_range);

    /* Spread step */
    RUN_TEST(test_spread_step_coprime);

    /* Constants */
    RUN_TEST(test_constants);

    return UNITY_END();
}
