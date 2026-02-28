/**
 * test_simd.c — SIMD detection, dispatch, and cross-path consistency tests.
 *
 * Tests:
 *
 * ## 1. SIMD detection
 *   1.1 netc_simd_detect() returns a valid level (GENERIC, SSE42, AVX2, or NEON)
 *   1.2 Context created with simd_level=0 (auto) uses best available path
 *   1.3 Context created with simd_level=1 (generic) forces generic path
 *   1.4 Manual override: generic path on AVX2 CPU still produces correct output
 *
 * ## 2. Generic path correctness
 *   2.1 generic delta encode/decode round-trip (all field-class regions)
 *   2.2 generic freq_count produces correct histogram
 *   2.3 generic crc32_update matches known CRC32 value
 *
 * ## 3. Cross-path output consistency (spec: byte-for-byte identical)
 *   3.1 SSE4.2 delta encode == generic delta encode
 *   3.2 SSE4.2 delta decode == generic delta decode
 *   3.3 SSE4.2 freq_count == generic freq_count
 *   3.4 AVX2 delta encode == generic delta encode (if AVX2 available)
 *   3.5 AVX2 delta decode == generic delta decode (if AVX2 available)
 *   3.6 AVX2 freq_count == generic freq_count (if AVX2 available)
 *
 * ## 4. Unaligned buffer safety
 *   4.1 SSE4.2 delta encode on buffer starting at odd address — no fault
 *   4.2 SSE4.2 delta decode on buffer starting at odd address — no fault
 *   4.3 AVX2 delta encode on unaligned buffer — no fault
 *
 * ## 5. Pipeline integration with SIMD
 *   5.1 Compress/decompress with auto SIMD → correct round-trip
 *   5.2 Compress with AVX2 ctx, decompress with generic ctx → correct round-trip
 *   5.3 Compress with generic ctx, decompress with auto ctx → correct round-trip
 *
 * ## 6. Spec scenarios
 *   6.1 Graceful fallback: forcing generic level always works
 *   6.2 Dispatch table level field matches selected level
 */

#include "unity.h"
#include "netc.h"
#include "simd/netc_simd.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Alignment padding for unaligned buffer tests */
#define ALIGN_OVERHEAD 64

/* =========================================================================
 * Test fixtures
 * ========================================================================= */

#define PKT_SIZE 512

static uint8_t s_prev[PKT_SIZE];
static uint8_t s_curr[PKT_SIZE];
static netc_dict_t *s_dict = NULL;

/* Training data */
static uint8_t s_train[8][PKT_SIZE];

void setUp(void) {
    for (int i = 0; i < PKT_SIZE; i++) {
        s_prev[i] = (uint8_t)(i & 0xFF);
        s_curr[i] = (uint8_t)((i + 37) & 0xFF);
    }

    /* Build training corpus */
    const uint8_t *ptrs[8];
    size_t szs[8];
    for (int p = 0; p < 8; p++) {
        for (int i = 0; i < PKT_SIZE; i++) {
            s_train[p][i] = (uint8_t)(0x41 + (i & 0x0F) + p);
        }
        ptrs[p] = s_train[p];
        szs[p]  = PKT_SIZE;
    }
    netc_dict_train(ptrs, szs, 8, 3, &s_dict);
}

void tearDown(void) {
    netc_dict_free(s_dict);
    s_dict = NULL;
}

/* =========================================================================
 * Helper: create a context with given simd_level and optional delta
 * ========================================================================= */
static netc_ctx_t *make_ctx(uint8_t simd_level, int delta) {
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags      = NETC_CFG_FLAG_STATEFUL | (delta ? NETC_CFG_FLAG_DELTA : 0);
    cfg.simd_level = simd_level;
    return netc_ctx_create(s_dict, &cfg);
}

/* =========================================================================
 * 1. SIMD detection
 * ========================================================================= */

void test_simd_detect_valid_level(void) {
    /* 1.1 detect() returns a recognized level */
    uint8_t level = netc_simd_detect();
    TEST_ASSERT_TRUE_MESSAGE(
        level == NETC_SIMD_LEVEL_GENERIC ||
        level == NETC_SIMD_LEVEL_SSE42   ||
        level == NETC_SIMD_LEVEL_AVX2    ||
        level == NETC_SIMD_LEVEL_NEON,
        "simd_detect returns unrecognized level"
    );
}

void test_simd_auto_ctx_has_ops(void) {
    /* 1.2 Auto-created context has non-NULL function pointers */
    netc_ctx_t *ctx = make_ctx(NETC_SIMD_LEVEL_AUTO, 0);
    TEST_ASSERT_NOT_NULL(ctx);

    /* Access simd_ops via a compress call (just verify no crash) */
    uint8_t src[64], dst[128];
    size_t dsz;
    memset(src, 0x41, 64);
    netc_result_t r = netc_compress(ctx, src, 64, dst, 128, &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_ctx_destroy(ctx);
}

void test_simd_generic_override(void) {
    /* 1.3 simd_level=1 forces generic path */
    netc_simd_ops_t ops;
    netc_simd_ops_init(&ops, NETC_SIMD_LEVEL_GENERIC);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(NETC_SIMD_LEVEL_GENERIC, ops.level,
                                    "forced generic: level field");
    TEST_ASSERT_EQUAL_PTR_MESSAGE((void *)netc_delta_encode_generic,
                                  (void *)ops.delta_encode,
                                  "forced generic: delta_encode ptr");
}

void test_simd_dispatch_level_field(void) {
    /* 6.2 Dispatch table level field matches selected level */
    netc_simd_ops_t ops_auto, ops_generic;
    netc_simd_ops_init(&ops_auto,    NETC_SIMD_LEVEL_AUTO);
    netc_simd_ops_init(&ops_generic, NETC_SIMD_LEVEL_GENERIC);

    TEST_ASSERT_EQUAL_UINT8(NETC_SIMD_LEVEL_GENERIC, ops_generic.level);
    /* Auto should be one of the valid levels */
    TEST_ASSERT_TRUE(ops_auto.level >= NETC_SIMD_LEVEL_GENERIC &&
                     ops_auto.level <= NETC_SIMD_LEVEL_NEON);
}

/* =========================================================================
 * 2. Generic path correctness
 * ========================================================================= */

void test_generic_delta_roundtrip(void) {
    /* 2.1 generic delta encode/decode round-trip */
    uint8_t residual[PKT_SIZE], recovered[PKT_SIZE];

    netc_delta_encode_generic(s_prev, s_curr, residual, PKT_SIZE);
    netc_delta_decode_generic(s_prev, residual, recovered, PKT_SIZE);

    for (int i = 0; i < PKT_SIZE; i++) {
        if (s_curr[i] != recovered[i]) {
            char msg[64];
            snprintf(msg, sizeof(msg), "generic delta roundtrip: mismatch at %d", i);
            TEST_FAIL_MESSAGE(msg);
            return;
        }
    }
}

void test_generic_freq_count(void) {
    /* 2.2 generic freq_count: verify known pattern */
    uint8_t data[256];
    uint32_t freq[256] = {0};

    /* Each byte value appears exactly once */
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;

    netc_freq_count_generic(data, 256, freq);

    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, freq[i], "generic freq: each byte appears once");
    }
}

void test_generic_freq_count_accumulates(void) {
    /* freq_count ADDS to existing counts */
    uint8_t data[4] = {0x01, 0x01, 0x02, 0x03};
    uint32_t freq[256] = {0};
    freq[1] = 10;  /* pre-existing count */

    netc_freq_count_generic(data, 4, freq);

    TEST_ASSERT_EQUAL_UINT32(12, freq[1]);  /* 10 + 2 */
    TEST_ASSERT_EQUAL_UINT32(1,  freq[2]);
    TEST_ASSERT_EQUAL_UINT32(1,  freq[3]);
}

void test_generic_crc32_known_value(void) {
    /* 2.3 CRC32 of empty data — pass 0 as initial value (function handles complement)
     * CRC32/ISO-HDLC of "" = 0x00000000 */
    uint8_t empty = 0;
    uint32_t crc = netc_crc32_update_generic(0, &empty, 0);
    TEST_ASSERT_EQUAL_HEX32(0x00000000U, crc);
}

void test_generic_crc32_abc(void) {
    /* CRC32/ISO-HDLC of "123456789" = 0xCBF43926 (standard test vector)
     * Initial value 0 (function applies ~crc internally). */
    const uint8_t data[] = {'1','2','3','4','5','6','7','8','9'};
    uint32_t crc = netc_crc32_update_generic(0, data, 9);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc);
}

/* =========================================================================
 * 3. Cross-path output consistency
 * ========================================================================= */

static void assert_delta_paths_equal(
    netc_delta_encode_fn enc_a,
    netc_delta_encode_fn enc_b,
    netc_delta_decode_fn dec_a,
    netc_delta_decode_fn dec_b,
    const char *name_a,
    const char *name_b)
{
    uint8_t res_a[PKT_SIZE], res_b[PKT_SIZE];
    uint8_t rec_a[PKT_SIZE], rec_b[PKT_SIZE];

    enc_a(s_prev, s_curr, res_a, PKT_SIZE);
    enc_b(s_prev, s_curr, res_b, PKT_SIZE);

    for (int i = 0; i < PKT_SIZE; i++) {
        if (res_a[i] != res_b[i]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "encode mismatch %s vs %s at offset %d",
                     name_a, name_b, i);
            TEST_FAIL_MESSAGE(msg);
            return;
        }
    }

    dec_a(s_prev, res_a, rec_a, PKT_SIZE);
    dec_b(s_prev, res_b, rec_b, PKT_SIZE);

    for (int i = 0; i < PKT_SIZE; i++) {
        if (rec_a[i] != rec_b[i]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "decode mismatch %s vs %s at offset %d",
                     name_a, name_b, i);
            TEST_FAIL_MESSAGE(msg);
            return;
        }
    }
}

void test_sse42_delta_matches_generic(void) {
    /* 3.1/3.2 SSE4.2 encode+decode == generic */
    assert_delta_paths_equal(
        netc_delta_encode_generic, netc_delta_encode_sse42,
        netc_delta_decode_generic, netc_delta_decode_sse42,
        "generic", "sse42"
    );
}

void test_avx2_delta_matches_generic(void) {
    /* 3.4/3.5 AVX2 encode+decode == generic (always compiled on MSVC x64) */
    assert_delta_paths_equal(
        netc_delta_encode_generic, netc_delta_encode_avx2,
        netc_delta_decode_generic, netc_delta_decode_avx2,
        "generic", "avx2"
    );
}

void test_sse42_freq_matches_generic(void) {
    /* 3.3 SSE4.2 freq_count == generic freq_count */
    uint32_t freq_gen[256] = {0};
    uint32_t freq_sse[256] = {0};

    netc_freq_count_generic(s_curr, PKT_SIZE, freq_gen);
    netc_freq_count_sse42  (s_curr, PKT_SIZE, freq_sse);

    for (int i = 0; i < 256; i++) {
        if (freq_gen[i] != freq_sse[i]) {
            char msg[64];
            snprintf(msg, sizeof(msg), "sse42 freq mismatch at symbol 0x%02X", i);
            TEST_FAIL_MESSAGE(msg);
            return;
        }
    }
}

void test_avx2_freq_matches_generic(void) {
    /* 3.6 AVX2 freq_count == generic */
    uint32_t freq_gen[256] = {0};
    uint32_t freq_avx[256] = {0};

    netc_freq_count_generic(s_curr, PKT_SIZE, freq_gen);
    netc_freq_count_avx2   (s_curr, PKT_SIZE, freq_avx);

    for (int i = 0; i < 256; i++) {
        if (freq_gen[i] != freq_avx[i]) {
            char msg[64];
            snprintf(msg, sizeof(msg), "avx2 freq mismatch at symbol 0x%02X", i);
            TEST_FAIL_MESSAGE(msg);
            return;
        }
    }
}

void test_sse42_crc32_matches_generic(void) {
    /* 3.7 SSE4.2 crc32_update produces identical output to generic
     * (both must use IEEE CRC32, not CRC32C) */
    const uint8_t test_vec[] = {'1','2','3','4','5','6','7','8','9'};
    uint32_t crc_gen  = netc_crc32_update_generic(0, test_vec, 9);
    uint32_t crc_sse  = netc_crc32_update_sse42(0, test_vec, 9);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc_gen);
    TEST_ASSERT_EQUAL_HEX32(crc_gen, crc_sse);

    /* Also test with random data */
    uint32_t crc_gen2 = netc_crc32_update_generic(0, s_curr, PKT_SIZE);
    uint32_t crc_sse2 = netc_crc32_update_sse42(0, s_curr, PKT_SIZE);
    TEST_ASSERT_EQUAL_HEX32(crc_gen2, crc_sse2);
}

void test_dict_crc32_roundtrip(void) {
    /* 3.8 Dictionary train → save → load round-trip with CRC32 validation.
     * Verifies that the CRC32 computed at save time matches at load time,
     * regardless of which SIMD path is active. */
    TEST_ASSERT_NOT_NULL(s_dict);

    /* Save the dict to a blob */
    void  *blob = NULL;
    size_t blob_size = 0;
    netc_result_t r = netc_dict_save(s_dict, &blob, &blob_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(blob);
    TEST_ASSERT_GREATER_THAN(0U, blob_size);

    /* Load it back — this validates the CRC32 checksum internally */
    netc_dict_t *loaded = NULL;
    r = netc_dict_load(blob, blob_size, &loaded);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(loaded);

    netc_dict_free(loaded);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * 4. Unaligned buffer safety
 * ========================================================================= */

void test_sse42_unaligned_encode(void) {
    /* 4.1 SSE4.2 delta encode on buffer at odd address — no fault */
    uint8_t *heap = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    TEST_ASSERT_NOT_NULL(heap);

    /* Force odd alignment offset +1 */
    uint8_t *unaligned_prev = heap + 1;
    uint8_t *unaligned_curr = heap + 1 + PKT_SIZE / 2;  /* different offset */
    /* This won't work for two buffers sharing the heap. Use separate allocs. */
    uint8_t *heap2 = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    uint8_t *heap3 = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    TEST_ASSERT_NOT_NULL(heap2);
    TEST_ASSERT_NOT_NULL(heap3);

    uint8_t *up = heap2 + 1;   /* unaligned prev */
    uint8_t *uc = heap3 + 1;   /* unaligned curr */
    uint8_t out[PKT_SIZE];

    memcpy(up, s_prev, PKT_SIZE);
    memcpy(uc, s_curr, PKT_SIZE);

    /* Must not crash or fault */
    netc_delta_encode_sse42(up, uc, out, PKT_SIZE);

    /* Verify output matches aligned reference */
    uint8_t ref[PKT_SIZE];
    netc_delta_encode_generic(s_prev, s_curr, ref, PKT_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, PKT_SIZE);

    free(heap); free(heap2); free(heap3);
    (void)unaligned_prev; (void)unaligned_curr;
}

void test_sse42_unaligned_decode(void) {
    /* 4.2 SSE4.2 delta decode on unaligned buffer */
    uint8_t residual[PKT_SIZE];
    netc_delta_encode_generic(s_prev, s_curr, residual, PKT_SIZE);

    uint8_t *heap_p = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    uint8_t *heap_r = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    TEST_ASSERT_NOT_NULL(heap_p);
    TEST_ASSERT_NOT_NULL(heap_r);

    uint8_t *up = heap_p + 3;  /* +3 bytes off natural alignment */
    uint8_t *ur = heap_r + 5;
    uint8_t out[PKT_SIZE];

    memcpy(up, s_prev,   PKT_SIZE);
    memcpy(ur, residual, PKT_SIZE);

    netc_delta_decode_sse42(up, ur, out, PKT_SIZE);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_curr, out, PKT_SIZE);

    free(heap_p); free(heap_r);
}

void test_avx2_unaligned_encode(void) {
    /* 4.3 AVX2 delta encode on unaligned buffer */
    uint8_t *hp = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    uint8_t *hc = (uint8_t *)malloc(PKT_SIZE + ALIGN_OVERHEAD);
    TEST_ASSERT_NOT_NULL(hp);
    TEST_ASSERT_NOT_NULL(hc);

    uint8_t *up = hp + 7;
    uint8_t *uc = hc + 7;
    uint8_t out[PKT_SIZE];

    memcpy(up, s_prev, PKT_SIZE);
    memcpy(uc, s_curr, PKT_SIZE);

    netc_delta_encode_avx2(up, uc, out, PKT_SIZE);

    uint8_t ref[PKT_SIZE];
    netc_delta_encode_generic(s_prev, s_curr, ref, PKT_SIZE);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, out, PKT_SIZE);

    free(hp); free(hc);
}

/* =========================================================================
 * 5. Pipeline integration with SIMD
 * ========================================================================= */

void test_simd_pipeline_auto_roundtrip(void) {
    /* 5.1 Compress/decompress with auto SIMD path */
    TEST_ASSERT_NOT_NULL(s_dict);

    netc_ctx_t *ctx = make_ctx(NETC_SIMD_LEVEL_AUTO, 1);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t cbuf1[PKT_SIZE + 64], cbuf2[PKT_SIZE + 64];
    uint8_t dbuf1[PKT_SIZE], dbuf2[PKT_SIZE];
    size_t csz1, csz2, dsz1, dsz2;

    netc_result_t r = netc_compress(ctx, s_prev, PKT_SIZE, cbuf1, sizeof(cbuf1), &csz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    r = netc_compress(ctx, s_curr, PKT_SIZE, cbuf2, sizeof(cbuf2), &csz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_ctx_reset(ctx);

    r = netc_decompress(ctx, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(PKT_SIZE, dsz1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_prev, dbuf1, PKT_SIZE);

    r = netc_decompress(ctx, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(PKT_SIZE, dsz2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_curr, dbuf2, PKT_SIZE);

    netc_ctx_destroy(ctx);
}

void test_simd_pipeline_cross_path(void) {
    /* 5.2 Compress with AVX2 ctx, decompress with generic ctx */
    TEST_ASSERT_NOT_NULL(s_dict);

    netc_ctx_t *enc = make_ctx(NETC_SIMD_LEVEL_AVX2,    1);
    netc_ctx_t *dec = make_ctx(NETC_SIMD_LEVEL_GENERIC, 1);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t cbuf1[PKT_SIZE + 64], cbuf2[PKT_SIZE + 64];
    size_t csz1, csz2;

    netc_compress(enc, s_prev, PKT_SIZE, cbuf1, sizeof(cbuf1), &csz1);
    netc_compress(enc, s_curr, PKT_SIZE, cbuf2, sizeof(cbuf2), &csz2);

    uint8_t dbuf1[PKT_SIZE], dbuf2[PKT_SIZE];
    size_t dsz1, dsz2;

    netc_result_t r = netc_decompress(dec, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_prev, dbuf1, PKT_SIZE);

    r = netc_decompress(dec, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_curr, dbuf2, PKT_SIZE);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
}

void test_simd_pipeline_generic_enc_auto_dec(void) {
    /* 5.3 Compress with generic, decompress with auto */
    TEST_ASSERT_NOT_NULL(s_dict);

    netc_ctx_t *enc = make_ctx(NETC_SIMD_LEVEL_GENERIC, 1);
    netc_ctx_t *dec = make_ctx(NETC_SIMD_LEVEL_AUTO,    1);
    TEST_ASSERT_NOT_NULL(enc);
    TEST_ASSERT_NOT_NULL(dec);

    uint8_t cbuf1[PKT_SIZE + 64], cbuf2[PKT_SIZE + 64];
    size_t csz1, csz2;

    netc_compress(enc, s_prev, PKT_SIZE, cbuf1, sizeof(cbuf1), &csz1);
    netc_compress(enc, s_curr, PKT_SIZE, cbuf2, sizeof(cbuf2), &csz2);

    uint8_t dbuf1[PKT_SIZE], dbuf2[PKT_SIZE];
    size_t dsz1, dsz2;

    netc_result_t r = netc_decompress(dec, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_prev, dbuf1, PKT_SIZE);

    r = netc_decompress(dec, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_curr, dbuf2, PKT_SIZE);

    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
}

/* =========================================================================
 * 6. Spec scenarios
 * ========================================================================= */

void test_simd_graceful_fallback(void) {
    /* 6.1 Forcing GENERIC always works and produces correct output */
    netc_simd_ops_t ops;
    netc_simd_ops_init(&ops, NETC_SIMD_LEVEL_GENERIC);

    uint8_t res[PKT_SIZE], rec[PKT_SIZE];
    ops.delta_encode(s_prev, s_curr, res, PKT_SIZE);
    ops.delta_decode(s_prev, res,    rec, PKT_SIZE);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_curr, rec, PKT_SIZE);
}

void test_simd_cross_path_small_packet(void) {
    /* Cross-path consistency on a small packet (< 16 bytes, all scalar tail) */
    uint8_t prev8[8] = {0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80};
    uint8_t curr8[8] = {0x11,0x21,0x31,0x41,0x51,0x61,0x71,0x81};
    uint8_t res_gen[8], res_sse[8], res_avx[8];
    uint8_t rec_gen[8], rec_sse[8], rec_avx[8];

    netc_delta_encode_generic(prev8, curr8, res_gen, 8);
    netc_delta_encode_sse42  (prev8, curr8, res_sse, 8);
    netc_delta_encode_avx2   (prev8, curr8, res_avx, 8);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(res_gen, res_sse, 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(res_gen, res_avx, 8);

    netc_delta_decode_generic(prev8, res_gen, rec_gen, 8);
    netc_delta_decode_sse42  (prev8, res_sse, rec_sse, 8);
    netc_delta_decode_avx2   (prev8, res_avx, rec_avx, 8);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(curr8, rec_gen, 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(curr8, rec_sse, 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(curr8, rec_avx, 8);
}

void test_simd_cross_path_boundary_packet(void) {
    /* Cross-path at exact field-class boundary sizes (17, 65, 257 bytes) */
    static const size_t sizes[] = {17, 65, 257};
    for (int s = 0; s < 3; s++) {
        size_t n = sizes[s];
        uint8_t res_gen[300], res_sse[300], res_avx[300];
        uint8_t rec_gen[300], rec_sse[300];

        netc_delta_encode_generic(s_prev, s_curr, res_gen, n);
        netc_delta_encode_sse42  (s_prev, s_curr, res_sse, n);
        netc_delta_encode_avx2   (s_prev, s_curr, res_avx, n);

        for (size_t i = 0; i < n; i++) {
            if (res_gen[i] != res_sse[i]) {
                char msg[64];
                snprintf(msg, sizeof(msg), "sse42 encode mismatch at n=%zu i=%zu",
                         n, i);
                TEST_FAIL_MESSAGE(msg);
                return;
            }
            if (res_gen[i] != res_avx[i]) {
                char msg[64];
                snprintf(msg, sizeof(msg), "avx2 encode mismatch at n=%zu i=%zu",
                         n, i);
                TEST_FAIL_MESSAGE(msg);
                return;
            }
        }

        netc_delta_decode_generic(s_prev, res_gen, rec_gen, n);
        netc_delta_decode_sse42  (s_prev, res_sse, rec_sse, n);
        for (size_t i = 0; i < n; i++) {
            if (rec_gen[i] != (uint8_t)((s_curr[i]))) {
                TEST_FAIL_MESSAGE("generic decode wrong");
                return;
            }
            if (rec_sse[i] != rec_gen[i]) {
                TEST_FAIL_MESSAGE("sse42 decode mismatch");
                return;
            }
        }
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* 1. Detection */
    RUN_TEST(test_simd_detect_valid_level);
    RUN_TEST(test_simd_auto_ctx_has_ops);
    RUN_TEST(test_simd_generic_override);
    RUN_TEST(test_simd_dispatch_level_field);

    /* 2. Generic path correctness */
    RUN_TEST(test_generic_delta_roundtrip);
    RUN_TEST(test_generic_freq_count);
    RUN_TEST(test_generic_freq_count_accumulates);
    RUN_TEST(test_generic_crc32_known_value);
    RUN_TEST(test_generic_crc32_abc);

    /* 3. Cross-path consistency */
    RUN_TEST(test_sse42_delta_matches_generic);
    RUN_TEST(test_avx2_delta_matches_generic);
    RUN_TEST(test_sse42_freq_matches_generic);
    RUN_TEST(test_avx2_freq_matches_generic);

    /* 3b. CRC32 cross-path consistency */
    RUN_TEST(test_sse42_crc32_matches_generic);
    RUN_TEST(test_dict_crc32_roundtrip);

    /* 4. Unaligned buffers */
    RUN_TEST(test_sse42_unaligned_encode);
    RUN_TEST(test_sse42_unaligned_decode);
    RUN_TEST(test_avx2_unaligned_encode);

    /* 5. Pipeline integration */
    RUN_TEST(test_simd_pipeline_auto_roundtrip);
    RUN_TEST(test_simd_pipeline_cross_path);
    RUN_TEST(test_simd_pipeline_generic_enc_auto_dec);

    /* 6. Spec scenarios */
    RUN_TEST(test_simd_graceful_fallback);
    RUN_TEST(test_simd_cross_path_small_packet);
    RUN_TEST(test_simd_cross_path_boundary_packet);

    return UNITY_END();
}
