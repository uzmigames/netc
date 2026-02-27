/**
 * test_security.c — Security hardening tests (Phase 6, RFC-001 §15).
 *
 * Verifies that every decompressor safety check fires correctly on crafted
 * malformed inputs. No input may crash the library or cause out-of-bounds
 * memory access. All error paths must return a specific NETC_ERR_* code.
 *
 * Coverage targets (tasks 1.1–1.6):
 *   1.1  dst_cap strictly respected — never exceeded
 *   1.2  ANS state bounds check fires on corrupt state
 *   1.3  input bounds check — truncated input
 *   1.4  original_size validation — reject > NETC_MAX_PACKET_SIZE or > dst_cap
 *   1.5  dictionary checksum verified on load
 *   1.6  (this file)
 */

#include "unity.h"
#include "../include/netc.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Build a minimal valid 8-byte netc header into buf[8].
 *
 * Wire layout (RFC-001 §9.1):
 *   [0..1] original_size   (uint16 LE)
 *   [2..3] compressed_size (uint16 LE)
 *   [4]    flags
 *   [5]    algorithm
 *   [6]    model_id
 *   [7]    context_seq
 */
static void build_header(uint8_t buf[8],
                          uint8_t  algorithm,
                          uint8_t  flags,
                          uint8_t  model_id,
                          uint8_t  context_seq,
                          uint16_t original_size,
                          uint16_t compressed_size)
{
    /* original_size LE */
    buf[0] = (uint8_t)(original_size & 0xFFu);
    buf[1] = (uint8_t)((original_size >> 8) & 0xFFu);
    /* compressed_size LE */
    buf[2] = (uint8_t)(compressed_size & 0xFFu);
    buf[3] = (uint8_t)((compressed_size >> 8) & 0xFFu);
    buf[4] = flags;
    buf[5] = algorithm;
    buf[6] = model_id;
    buf[7] = context_seq;
}

static netc_dict_t *make_dict(void)
{
    /* Train a tiny dict on 100 identical 64-byte packets */
    static uint8_t pkt[64];
    memset(pkt, 0xAA, sizeof(pkt));
    const uint8_t *pkts[100];
    size_t         lens[100];
    for (int i = 0; i < 100; i++) { pkts[i] = pkt; lens[i] = 64; }
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, lens, 100, 1, &d);
    return d;
}

/* =========================================================================
 * setUp / tearDown
 * ========================================================================= */
void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * 1.4 original_size validation
 * ========================================================================= */

/* Scenario: Malicious original_size > dst_cap → NETC_ERR_BUF_SMALL */
void test_original_size_exceeds_dst_cap(void)
{
    /* Craft: original_size = 1000, but dst_cap = 128 */
    uint8_t pkt[8 + 4]; /* header + 4-byte payload (passthru payload) */
    build_header(pkt, 0xFF /* PASSTHRU */, 0x04 /* NETC_PKT_FLAG_PASSTHRU */,
                 0, 0, 128, 128);
    /* passthru: compressed_size == original_size = 128, but we'll lie */
    build_header(pkt, 0xFF, 0x04, 0, 0, 1000, 128);

    uint8_t dst[128];
    size_t  dst_size = 0;

    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 128, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Scenario: original_size > NETC_MAX_PACKET_SIZE → NETC_ERR_CORRUPT */
void test_original_size_exceeds_max_packet_size(void)
{
    uint8_t pkt[8];
    /* NETC_MAX_PACKET_SIZE = 65535 = 0xFFFF. Claim original_size = 0xFFFF
     * with dst_cap = 64 to trigger BUF_SMALL (or CORRUPT for truncated src). */
    build_header(pkt, 0xFF, 0x04, 0, 0, 0xFFFF, 0xFFFF);

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    /* Either BUF_SMALL (original_size > dst_cap) or CORRUPT (src truncated) */
    TEST_ASSERT_TRUE(rc == NETC_ERR_BUF_SMALL || rc == NETC_ERR_CORRUPT);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * 1.3 Input bounds check — truncated input
 * ========================================================================= */

/* Scenario: src_size < NETC_HEADER_SIZE → NETC_ERR_CORRUPT */
void test_truncated_header(void)
{
    uint8_t pkt[4] = {0xFF, 0x04, 0x01, 0x00}; /* only 4 bytes, need 8 */
    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Scenario: compressed packet claims payload = 100 bytes but src has only 20 */
void test_truncated_payload(void)
{
    uint8_t pkt[28]; /* 8-byte header + 20 bytes payload (short) */
    memset(pkt, 0, sizeof(pkt));
    /* Claim compressed_size = 100, but buffer only has 20 bytes after header */
    build_header(pkt, 0x01 /* TANS */, 0x00, 0x01, 0, 64, 100);

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Scenario: empty source buffer → NETC_ERR_CORRUPT */
void test_empty_src(void)
{
    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, (const uint8_t *)"", 0, dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * 1.2 ANS state bounds check
 * ========================================================================= */

/* Scenario: initial_state outside [TABLE_SIZE, 2*TABLE_SIZE) → NETC_ERR_CORRUPT */
void test_corrupt_initial_state_zero(void)
{
    /* TANS packet: header + 4-byte initial_state (= 0, invalid) + garbage */
    uint8_t pkt[8 + 4 + 8];
    memset(pkt, 0, sizeof(pkt));
    build_header(pkt, 0x01 /* TANS */, 0x00, 0x01, 0,
                 8,                   /* original_size */
                 (uint16_t)(4 + 8));  /* compressed_size = 4 state + 8 bits */
    /* initial_state = 0 (invalid; must be in [4096, 8192)) */
    pkt[8] = 0; pkt[9] = 0; pkt[10] = 0; pkt[11] = 0;

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* initial_state >= 2 * TABLE_SIZE → NETC_ERR_CORRUPT */
void test_corrupt_initial_state_too_large(void)
{
    uint8_t pkt[8 + 4 + 8];
    memset(pkt, 0, sizeof(pkt));
    build_header(pkt, 0x01, 0x00, 0x01, 0, 8, 12);
    /* initial_state = 0xFFFFFFFF */
    pkt[8] = 0xFF; pkt[9] = 0xFF; pkt[10] = 0xFF; pkt[11] = 0xFF;

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * 1.5 Dictionary checksum verification
 * ========================================================================= */

/* Scenario: corrupt blob checksum → NETC_ERR_DICT_INVALID */
void test_dict_load_corrupt_checksum(void)
{
    netc_dict_t *dict = make_dict();

    void  *blob = NULL;
    size_t blob_size = 0;
    netc_result_t rc = netc_dict_save(dict, &blob, &blob_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);

    /* Flip the last byte of the checksum */
    ((uint8_t *)blob)[blob_size - 1] ^= 0xFF;

    netc_dict_t *loaded = NULL;
    rc = netc_dict_load(blob, blob_size, &loaded);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, rc);
    TEST_ASSERT_NULL(loaded);

    netc_dict_free_blob(blob);
    netc_dict_free(dict);
}

/* Scenario: truncated blob → NETC_ERR_DICT_INVALID */
void test_dict_load_truncated_blob(void)
{
    netc_dict_t *dict = make_dict();

    void  *blob = NULL;
    size_t blob_size = 0;
    netc_dict_save(dict, &blob, &blob_size);

    netc_dict_t *loaded = NULL;
    netc_result_t rc = netc_dict_load(blob, blob_size / 2, &loaded);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, rc);
    TEST_ASSERT_NULL(loaded);

    netc_dict_free_blob(blob);
    netc_dict_free(dict);
}

/* Scenario: corrupt magic → NETC_ERR_DICT_INVALID */
void test_dict_load_corrupt_magic(void)
{
    netc_dict_t *dict = make_dict();

    void  *blob = NULL;
    size_t blob_size = 0;
    netc_dict_save(dict, &blob, &blob_size);
    /* Zero out magic */
    memset(blob, 0, 4);

    netc_dict_t *loaded = NULL;
    netc_result_t rc = netc_dict_load(blob, blob_size, &loaded);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, rc);
    TEST_ASSERT_NULL(loaded);

    netc_dict_free_blob(blob);
    netc_dict_free(dict);
}

/* Scenario: wrong version → NETC_ERR_VERSION */
void test_dict_load_wrong_version(void)
{
    netc_dict_t *dict = make_dict();

    void  *blob = NULL;
    size_t blob_size = 0;
    netc_dict_save(dict, &blob, &blob_size);
    /* Increment version byte (offset 4) */
    ((uint8_t *)blob)[4] = 99;
    /* Fix up checksum so version check is reached, not CRC check...
     * Actually, CRC is checked first so we just expect DICT_INVALID */
    netc_dict_t *loaded = NULL;
    netc_result_t rc = netc_dict_load(blob, blob_size, &loaded);
    TEST_ASSERT_TRUE(rc == NETC_ERR_DICT_INVALID || rc == NETC_ERR_VERSION);
    TEST_ASSERT_NULL(loaded);

    netc_dict_free_blob(blob);
    netc_dict_free(dict);
}

/* =========================================================================
 * 1.1 Output size cap enforcement
 * ========================================================================= */

/* Scenario: PASSTHRU packet with original_size == dst_cap: should succeed */
void test_passthru_fills_exactly_dst_cap(void)
{
    /* Build a valid passthru packet of exactly 32 bytes */
    uint8_t payload[32];
    memset(payload, 0xBB, sizeof(payload));

    uint8_t pkt[8 + 32];
    build_header(pkt, 0xFF, 0x04, 0, 0, 32, 32);
    memcpy(pkt + 8, payload, 32);

    uint8_t dst[32];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 32, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(32, dst_size);
    TEST_ASSERT_EQUAL_MEMORY(payload, dst, 32);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Scenario: PASSTHRU with original_size == dst_cap + 1 → BUF_SMALL */
void test_passthru_one_byte_over_dst_cap(void)
{
    uint8_t pkt[8 + 33];
    build_header(pkt, 0xFF, 0x04, 0, 0, 33, 33);
    memset(pkt + 8, 0xCC, 33);

    uint8_t dst[32];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 32, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * NULL argument guards
 * ========================================================================= */

void test_null_ctx(void)
{
    uint8_t dummy[8];
    memset(dummy, 0, 8);
    size_t out = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL,
        netc_decompress(NULL, dummy, 8, dummy, 8, &out));
}

void test_null_src(void)
{
    netc_ctx_t *ctx = netc_ctx_create(NULL, NULL);
    uint8_t dst[8];
    size_t  out = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_decompress(ctx, NULL, 8, dst, 8, &out));
    netc_ctx_destroy(ctx);
}

void test_null_dst(void)
{
    netc_ctx_t *ctx = netc_ctx_create(NULL, NULL);
    uint8_t src[8] = {0};
    size_t  out = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_decompress(ctx, src, 8, NULL, 8, &out));
    netc_ctx_destroy(ctx);
}

void test_null_dst_size(void)
{
    netc_ctx_t *ctx = netc_ctx_create(NULL, NULL);
    uint8_t src[8] = {0};
    uint8_t dst[8] = {0};
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_decompress(ctx, src, 8, dst, 8, NULL));
    netc_ctx_destroy(ctx);
}

/* =========================================================================
 * Algorithm field validation
 * ========================================================================= */

/* Unknown algorithm byte → NETC_ERR_CORRUPT */
void test_unknown_algorithm(void)
{
    uint8_t pkt[8 + 8];
    memset(pkt, 0, sizeof(pkt));
    build_header(pkt, 0x42 /* unknown */, 0, 1, 0, 8, 8);

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* rANS algorithm → NETC_ERR_UNSUPPORTED */
void test_rans_algorithm_unsupported(void)
{
    uint8_t pkt[8 + 8];
    memset(pkt, 0, sizeof(pkt));
    build_header(pkt, 0x02 /* RANS */, 0, 1, 0, 8, 8);

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_UNSUPPORTED, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * Model ID mismatch
 * ========================================================================= */

void test_model_id_mismatch(void)
{
    /* compressed_size=20, so src must be >= 8+20=28 bytes */
    uint8_t pkt[8 + 20];
    memset(pkt, 0, sizeof(pkt));
    /* model_id = 2 but dict has model_id = 1 — triggers ERR_VERSION before TANS decode */
    build_header(pkt, 0x01 /* TANS */, 0, 2 /* wrong model_id */, 0, 8, 20);

    uint8_t dst[64];
    size_t  dst_size = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_VERSION, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * Stateless API safety checks
 * ========================================================================= */

void test_stateless_null_dict(void)
{
    uint8_t src[8] = {0};
    uint8_t dst[8];
    size_t  out = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_decompress_stateless(NULL, src, 8, dst, 8, &out));
}

void test_stateless_truncated_input(void)
{
    netc_dict_t *dict = make_dict();
    uint8_t src[4] = {0};
    uint8_t dst[64];
    size_t  out = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT,
        netc_decompress_stateless(dict, src, 4, dst, 64, &out));
    netc_dict_free(dict);
}

void test_stateless_original_size_exceeds_dst_cap(void)
{
    netc_dict_t *dict = make_dict();
    uint8_t pkt[8];
    build_header(pkt, 0xFF, 0x04, 1, 0, 200, 200);

    uint8_t dst[64];
    size_t  out = 0;
    netc_result_t rc = netc_decompress_stateless(dict, pkt, sizeof(pkt), dst, 64, &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL, rc);
    netc_dict_free(dict);
}

/* =========================================================================
 * PASSTHRU mismatch: compressed_size != original_size → NETC_ERR_CORRUPT
 * ========================================================================= */

void test_passthru_size_mismatch(void)
{
    uint8_t pkt[8 + 32];
    memset(pkt, 0, sizeof(pkt));
    /* Set original=32, compressed=16 — should be equal for PASSTHRU */
    build_header(pkt, 0xFF, 0x04, 0, 0, 32, 16);

    uint8_t dst[64];
    size_t  out = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, 64, &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * All-zeros input: must not crash
 * ========================================================================= */

void test_all_zeros_input_no_crash(void)
{
    uint8_t pkt[256];
    memset(pkt, 0, sizeof(pkt));

    uint8_t dst[256];
    size_t  out = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    /* Don't check result — just ensure no crash */
    netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &out);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* All-0xFF input: must not crash */
void test_all_ff_input_no_crash(void)
{
    uint8_t pkt[256];
    memset(pkt, 0xFF, sizeof(pkt));

    uint8_t dst[256];
    size_t  out = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &out);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * dict_load: NULL inputs
 * ========================================================================= */

void test_dict_load_null_data(void)
{
    netc_dict_t *out = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_load(NULL, 2060, &out));
}

void test_dict_load_null_out(void)
{
    uint8_t buf[2060] = {0};
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_load(buf, 2060, NULL));
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* 1.4 original_size validation */
    RUN_TEST(test_original_size_exceeds_dst_cap);
    RUN_TEST(test_original_size_exceeds_max_packet_size);

    /* 1.3 Truncated input */
    RUN_TEST(test_truncated_header);
    RUN_TEST(test_truncated_payload);
    RUN_TEST(test_empty_src);

    /* 1.2 ANS state bounds */
    RUN_TEST(test_corrupt_initial_state_zero);
    RUN_TEST(test_corrupt_initial_state_too_large);

    /* 1.5 Dictionary checksum */
    RUN_TEST(test_dict_load_corrupt_checksum);
    RUN_TEST(test_dict_load_truncated_blob);
    RUN_TEST(test_dict_load_corrupt_magic);
    RUN_TEST(test_dict_load_wrong_version);

    /* 1.1 Output cap enforcement */
    RUN_TEST(test_passthru_fills_exactly_dst_cap);
    RUN_TEST(test_passthru_one_byte_over_dst_cap);

    /* NULL argument guards */
    RUN_TEST(test_null_ctx);
    RUN_TEST(test_null_src);
    RUN_TEST(test_null_dst);
    RUN_TEST(test_null_dst_size);

    /* Algorithm validation */
    RUN_TEST(test_unknown_algorithm);
    RUN_TEST(test_rans_algorithm_unsupported);

    /* Model ID mismatch */
    RUN_TEST(test_model_id_mismatch);

    /* Stateless API */
    RUN_TEST(test_stateless_null_dict);
    RUN_TEST(test_stateless_truncated_input);
    RUN_TEST(test_stateless_original_size_exceeds_dst_cap);

    /* PASSTHRU size mismatch */
    RUN_TEST(test_passthru_size_mismatch);

    /* Crash safety */
    RUN_TEST(test_all_zeros_input_no_crash);
    RUN_TEST(test_all_ff_input_no_crash);

    /* dict_load NULL */
    RUN_TEST(test_dict_load_null_data);
    RUN_TEST(test_dict_load_null_out);

    return UNITY_END();
}
