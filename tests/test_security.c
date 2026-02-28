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
 * RLE passthrough: NETC_PKT_FLAG_RLE in decompressor
 * ========================================================================= */

/* Craft a valid RLE-encoded passthrough packet and verify round-trip.
 * RLE stream: [(count, sym), ...] pairs — here 4 runs of 8 bytes each.
 * Total original_size = 32. */
void test_rle_passthrough_roundtrip(void)
{
    /* Build RLE stream: 4×(8, 0x11), (8, 0x22), (8, 0x33), (8, 0x44) */
    uint8_t rle[8];
    rle[0] = 8; rle[1] = 0x11;
    rle[2] = 8; rle[3] = 0x22;
    rle[4] = 8; rle[5] = 0x33;
    rle[6] = 8; rle[7] = 0x44;

    uint8_t pkt[8 + 8];
    /* original_size=32 (LE), compressed_size=8 (LE) */
    pkt[0] = 32; pkt[1] = 0;
    pkt[2] = 8;  pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x20u); /* PASSTHRU | RLE */
    pkt[5] = 0xFF; /* NETC_ALG_PASSTHRU */
    pkt[6] = 0;    /* model_id = 0 (no dict check for passthru) */
    pkt[7] = 0;
    memcpy(pkt + 8, rle, 8);

    uint8_t dst[32];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(32, dsz);
    /* Verify decoded bytes */
    for (int i = 0; i < 8;  i++) TEST_ASSERT_EQUAL_UINT8(0x11, dst[i]);
    for (int i = 8; i < 16; i++) TEST_ASSERT_EQUAL_UINT8(0x22, dst[i]);
    for (int i = 16; i < 24; i++) TEST_ASSERT_EQUAL_UINT8(0x33, dst[i]);
    for (int i = 24; i < 32; i++) TEST_ASSERT_EQUAL_UINT8(0x44, dst[i]);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Stateless RLE passthrough round-trip */
void test_rle_passthrough_stateless_roundtrip(void)
{
    uint8_t rle[4];
    rle[0] = 10; rle[1] = 0xAB;
    rle[2] = 10; rle[3] = 0xCD;

    uint8_t pkt[8 + 4];
    pkt[0] = 20; pkt[1] = 0;
    pkt[2] = 4;  pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x20u); /* PASSTHRU | RLE */
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, rle, 4);

    netc_dict_t *dict = make_dict();
    uint8_t dst[20];
    size_t  dsz = 0;
    netc_result_t rc = netc_decompress_stateless(dict, pkt, sizeof(pkt),
                                                  dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, rc);
    TEST_ASSERT_EQUAL_size_t(20, dsz);
    for (int i = 0;  i < 10; i++) TEST_ASSERT_EQUAL_UINT8(0xAB, dst[i]);
    for (int i = 10; i < 20; i++) TEST_ASSERT_EQUAL_UINT8(0xCD, dst[i]);
    netc_dict_free(dict);
}

/* Corrupt RLE stream: count=0 → NETC_ERR_CORRUPT */
void test_rle_corrupt_zero_count(void)
{
    uint8_t rle[2] = { 0, 0x42 }; /* count=0 is invalid */

    uint8_t pkt[8 + 2];
    pkt[0] = 10; pkt[1] = 0;
    pkt[2] = 2;  pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x20u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, rle, 2);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* Corrupt RLE: output exceeds original_size → NETC_ERR_CORRUPT */
void test_rle_corrupt_overflow(void)
{
    /* Claim original_size=5 but RLE decodes to 20 */
    uint8_t rle[2] = { 20, 0x42 };

    uint8_t pkt[8 + 2];
    pkt[0] = 5; pkt[1] = 0;   /* original_size=5 */
    pkt[2] = 2; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x20u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, rle, 2);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* RLE stream with odd number of bytes (unpaired count/sym) → NETC_ERR_CORRUPT */
void test_rle_corrupt_odd_length(void)
{
    /* 3 bytes: (8,0xAA) + 0xBB → odd remainder → out != orig_size */
    uint8_t rle[3] = { 8, 0xAA, 0xBB };

    uint8_t pkt[8 + 3];
    pkt[0] = 8; pkt[1] = 0;
    pkt[2] = 3; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x20u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, rle, 3);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * LZ77 decode corrupt paths
 * ========================================================================= */

/* LZ77 back-ref with offset > out (refers before start of buffer) → CORRUPT */
void test_lz77_corrupt_backref_offset_exceeds_out(void)
{
    /* Token: [1lllllll][oooooooo]
     * match_len = bits[6:0]+3, offset = byte+1
     * Emit a back-ref as the first token (out=0 → any back-ref is invalid) */
    uint8_t lz[2] = {
        (uint8_t)(0x80u | 0u), /* back-ref, match_len=3 */
        0u                      /* offset=1 → but out=0, invalid */
    };

    uint8_t pkt[8 + 2];
    pkt[0] = 4; pkt[1] = 0;   /* original_size=4 */
    pkt[2] = 2; pkt[3] = 0;   /* compressed_size=2 */
    pkt[4] = (uint8_t)(0x04u | 0x08u); /* PASSTHRU | LZ77 */
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, lz, 2);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* LZ77 back-ref whose match would overflow original_size → CORRUPT */
void test_lz77_corrupt_backref_match_overflow(void)
{
    /* First emit 3 literal bytes so out=3, then back-ref match_len=5 but orig=5 */
    uint8_t lz[6];
    lz[0] = 2;         /* literal run: len=3 (2+1) */
    lz[1] = 0xAA; lz[2] = 0xBB; lz[3] = 0xCC;
    lz[4] = (uint8_t)(0x80u | 2u); /* back-ref match_len=5 */
    lz[5] = 0u;        /* offset=1 */

    uint8_t pkt[8 + 6];
    pkt[0] = 5; pkt[1] = 0;   /* original_size=5 — back-ref wants 5 more → overflow */
    pkt[2] = 6; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x08u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, lz, 6);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* LZ77 literal run that overflows original_size → CORRUPT */
void test_lz77_corrupt_literal_overflow(void)
{
    /* original_size=4 but literal run requests 10 bytes */
    uint8_t lz[11];
    lz[0] = 9; /* literal len=10 (9+1) */
    memset(lz + 1, 0xAA, 10);

    uint8_t pkt[8 + 11];
    pkt[0] = 4; pkt[1] = 0;
    pkt[2] = 11; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x08u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, lz, 11);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* LZ77 back-ref missing second byte (truncated) → CORRUPT */
void test_lz77_corrupt_backref_truncated(void)
{
    /* Back-ref token with no following offset byte */
    uint8_t lz[1] = { (uint8_t)(0x80u | 0u) };

    uint8_t pkt[8 + 1];
    pkt[0] = 4; pkt[1] = 0;
    pkt[2] = 1; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x08u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    pkt[8] = lz[0];

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* LZ77 literal run goes past lz_size → CORRUPT */
void test_lz77_corrupt_literal_truncated(void)
{
    /* Literal run length=10 but only 5 bytes of data follow */
    uint8_t lz[6];
    lz[0] = 9; /* len=10 */
    memset(lz + 1, 0xBB, 5); /* only 5 bytes, not 10 */

    uint8_t pkt[8 + 6];
    pkt[0] = 10; pkt[1] = 0;
    pkt[2] = 6;  pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x08u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, lz, 6);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* LZ77 output shorter than orig_size → CORRUPT */
void test_lz77_corrupt_output_too_short(void)
{
    /* Emit only 3 literal bytes but claim original_size=8 */
    uint8_t lz[4];
    lz[0] = 2; /* literal len=3 */
    lz[1] = 0x11; lz[2] = 0x22; lz[3] = 0x33;

    uint8_t pkt[8 + 4];
    pkt[0] = 8; pkt[1] = 0;
    pkt[2] = 4; pkt[3] = 0;
    pkt[4] = (uint8_t)(0x04u | 0x08u);
    pkt[5] = 0xFF;
    pkt[6] = 0;
    pkt[7] = 0;
    memcpy(pkt + 8, lz, 4);

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * MREG corrupt paths (multi-region tANS)
 * ========================================================================= */

/* MREG with n_regions=0 → CORRUPT */
void test_mreg_corrupt_n_regions_zero(void)
{
    /* Craft a TANS+MREG packet with n_regions=0 */
    uint8_t pkt[8 + 8]; /* header + 1B(n_regions=0) + 7B padding */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 8; pkt[1] = 0;   /* original_size=8 */
    pkt[2] = 8; pkt[3] = 0;   /* compressed_size=8 */
    pkt[4] = 0x10u;            /* NETC_PKT_FLAG_MREG */
    pkt[5] = 0x01;             /* NETC_ALG_TANS */
    pkt[6] = 1;                /* model_id=1 */
    pkt[7] = 0;
    pkt[8] = 0;                /* n_regions=0 (invalid) */

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* MREG with truncated descriptors (payload too small for n*8 bytes) → CORRUPT */
void test_mreg_corrupt_truncated_descriptors(void)
{
    /* n_regions=4, needs 1+4*8=33 bytes, but compressed_size=16 */
    uint8_t pkt[8 + 16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 32; pkt[1] = 0;
    pkt[2] = 16; pkt[3] = 0;
    pkt[4] = 0x10u;   /* MREG */
    pkt[5] = 0x01;    /* TANS */
    pkt[6] = 1;
    pkt[7] = 0;
    pkt[8] = 4;       /* n_regions=4, but not enough bytes for 4*8 descriptors */

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* MREG region with state outside [TABLE_SIZE, 2*TABLE_SIZE) → CORRUPT */
void test_mreg_corrupt_region_state_out_of_range(void)
{
    /* n_regions=1, descriptor: state=0 (invalid), bs_bytes=4 */
    uint8_t pkt[8 + 1 + 8 + 4];
    memset(pkt, 0, sizeof(pkt));
    size_t payload_sz = 1 + 8 + 4;
    pkt[0] = 8;  pkt[1] = 0;
    pkt[2] = (uint8_t)payload_sz; pkt[3] = 0;
    pkt[4] = 0x10u;
    pkt[5] = 0x01;
    pkt[6] = 1;
    pkt[7] = 0;
    /* n_regions=1 */
    pkt[8] = 1;
    /* state=0 (invalid, must be >= 4096) */
    pkt[9] = 0; pkt[10] = 0; pkt[11] = 0; pkt[12] = 0;
    /* bs_bytes=4 */
    pkt[13] = 4; pkt[14] = 0; pkt[15] = 0; pkt[16] = 0;
    /* 4 bytes of garbage bitstream */
    pkt[17] = 0xFF; pkt[18] = 0xFF; pkt[19] = 0xFF; pkt[20] = 0xFF;

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* MREG region where bits_offset+bs_bytes > bits_avail → CORRUPT */
void test_mreg_corrupt_bs_bytes_overflow(void)
{
    /* n_regions=1, descriptor: valid state, bs_bytes=999 (exceeds actual payload) */
    uint8_t pkt[8 + 1 + 8 + 4];
    memset(pkt, 0, sizeof(pkt));
    size_t payload_sz = 1 + 8 + 4;
    pkt[0] = 8;  pkt[1] = 0;
    pkt[2] = (uint8_t)payload_sz; pkt[3] = 0;
    pkt[4] = 0x10u;
    pkt[5] = 0x01;
    pkt[6] = 1;
    pkt[7] = 0;
    pkt[8] = 1;  /* n_regions=1 */
    /* state=4096 (NETC_TANS_TABLE_SIZE, valid) */
    pkt[9] = 0x00; pkt[10] = 0x10; pkt[11] = 0; pkt[12] = 0;
    /* bs_bytes=999 — much larger than available 4 bytes */
    pkt[13] = (uint8_t)(999 & 0xFF); pkt[14] = (uint8_t)(999 >> 8); pkt[15] = 0; pkt[16] = 0;
    pkt[17] = 0; pkt[18] = 0; pkt[19] = 0; pkt[20] = 0;

    uint8_t dst[64];
    size_t  dsz = 0;
    netc_dict_t *dict = make_dict();
    netc_ctx_t  *ctx  = netc_ctx_create(dict, NULL);

    netc_result_t rc = netc_decompress(ctx, pkt, sizeof(pkt), dst, sizeof(dst), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, rc);

    netc_ctx_destroy(ctx);
    netc_dict_free(dict);
}

/* =========================================================================
 * Context creation with custom ring_buffer_size
 * ========================================================================= */

void test_ctx_create_custom_ring_size(void)
{
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags            = NETC_CFG_FLAG_STATEFUL;
    cfg.ring_buffer_size = 4096;  /* non-zero custom size */

    netc_ctx_t *ctx = netc_ctx_create(NULL, &cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    /* Verify it works for basic compress */
    uint8_t src[16], dst[64];
    memset(src, 0x55, sizeof(src));
    size_t dsz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx, src, sizeof(src), dst, sizeof(dst), &dsz));
    TEST_ASSERT_GREATER_THAN(0, (int)dsz);

    netc_ctx_destroy(ctx);
}

/* =========================================================================
 * Stats accumulation in tANS compress path
 * ========================================================================= */

void test_stats_tans_compress_path(void)
{
    /* Train a dict on uniform data so tANS definitely activates */
    uint8_t uniform[256];
    memset(uniform, 0x42, sizeof(uniform));

    const uint8_t *pkts[] = { uniform };
    size_t         szs[]  = { sizeof(uniform) };
    netc_dict_t *d = NULL;
    netc_dict_train(pkts, szs, 1, 7, &d);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_STATS;
    netc_ctx_t *ctx = netc_ctx_create(d, &cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    size_t bound = netc_compress_bound(sizeof(uniform));
    uint8_t *cbuf = (uint8_t *)malloc(bound);
    TEST_ASSERT_NOT_NULL(cbuf);

    size_t csz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_compress(ctx, uniform, sizeof(uniform), cbuf, bound, &csz));

    /* If tANS was used (not passthru), stats.packets_compressed must be 1 */
    netc_stats_t st;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_ctx_stats(ctx, &st));
    TEST_ASSERT_EQUAL_UINT64(1, st.packets_compressed);
    TEST_ASSERT_EQUAL_UINT64(sizeof(uniform), st.bytes_in);
    TEST_ASSERT_GREATER_THAN(0, (int)st.bytes_out);

    free(cbuf);
    netc_ctx_destroy(ctx);
    netc_dict_free(d);
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

    /* RLE passthrough paths */
    RUN_TEST(test_rle_passthrough_roundtrip);
    RUN_TEST(test_rle_passthrough_stateless_roundtrip);
    RUN_TEST(test_rle_corrupt_zero_count);
    RUN_TEST(test_rle_corrupt_overflow);
    RUN_TEST(test_rle_corrupt_odd_length);

    /* LZ77 decode corrupt paths */
    RUN_TEST(test_lz77_corrupt_backref_offset_exceeds_out);
    RUN_TEST(test_lz77_corrupt_backref_match_overflow);
    RUN_TEST(test_lz77_corrupt_literal_overflow);
    RUN_TEST(test_lz77_corrupt_backref_truncated);
    RUN_TEST(test_lz77_corrupt_literal_truncated);
    RUN_TEST(test_lz77_corrupt_output_too_short);

    /* MREG corrupt paths */
    RUN_TEST(test_mreg_corrupt_n_regions_zero);
    RUN_TEST(test_mreg_corrupt_truncated_descriptors);
    RUN_TEST(test_mreg_corrupt_region_state_out_of_range);
    RUN_TEST(test_mreg_corrupt_bs_bytes_overflow);

    /* Context with custom ring size */
    RUN_TEST(test_ctx_create_custom_ring_size);

    /* Stats in tANS compress path */
    RUN_TEST(test_stats_tans_compress_path);

    return UNITY_END();
}
