/**
 * test_dict.c — Unit tests for dictionary training, serialization, and loading.
 *
 * Tests:
 *   Training:
 *     - NULL out_dict → NETC_ERR_INVALID_ARG
 *     - Reserved model_id 0 and 255 → NETC_ERR_INVALID_ARG
 *     - NULL packets with count > 0 → NETC_ERR_INVALID_ARG
 *     - Zero packets (empty training) → NETC_OK (uniform distribution)
 *     - Single packet training → NETC_OK, tables valid
 *     - Multiple packet training → NETC_OK, tables valid
 *     - Correct model_id stored
 *   Normalization:
 *     - Frequency tables sum to TABLE_SIZE after training
 *   Serialization (save):
 *     - NULL args → NETC_ERR_INVALID_ARG
 *     - Blob size equals expected DICT_BLOB_SIZE (40972 bytes, v0.3 with 16 buckets + 4 bigram classes)
 *     - Magic and model_id readable from blob
 *   Deserialization (load):
 *     - NULL args → NETC_ERR_INVALID_ARG
 *     - Short blob → NETC_ERR_DICT_INVALID
 *     - Wrong magic → NETC_ERR_DICT_INVALID
 *     - Wrong version → NETC_ERR_VERSION
 *     - Corrupt checksum → NETC_ERR_DICT_INVALID
 *     - Round-trip: train → save → load → tables valid
 *   model_id accessor:
 *     - NULL dict → returns 0
 *     - Valid dict → returns correct model_id
 */

#include "unity.h"
#include "netc.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Match the blob size from netc_dict.c */
#define NETC_CTX_COUNT        16U
#define NETC_BIGRAM_CTX_COUNT  4U
#define NETC_TANS_SYMBOLS     256U
#define NETC_TANS_TABLE_SIZE  4096U
#define NETC_LZP_HT_SIZE     131072U
/* v3 (no LZP): 8 + 16*256*2 + 16*4*256*2 + 4 = 40972 */
#define EXPECTED_BLOB_SIZE_V3  (8U + NETC_CTX_COUNT * NETC_TANS_SYMBOLS * 2U + \
                                NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT * NETC_TANS_SYMBOLS * 2U + 4U)
/* v4 with LZP: base(40968) + 4 + 131072*2 + 4 = 303120 */
#define EXPECTED_BLOB_SIZE_V4  (8U + NETC_CTX_COUNT * NETC_TANS_SYMBOLS * 2U + \
                                NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT * NETC_TANS_SYMBOLS * 2U + \
                                4U + NETC_LZP_HT_SIZE * 2U + 4U)

/* =========================================================================
 * Sample training data — representative byte sequences
 * ========================================================================= */

static const uint8_t PKT_A[64] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
};

static const uint8_t PKT_B[32] = {
    0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
    0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,
    0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11,
    0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99
};

static const uint8_t PKT_C[256] = {
    /* Repeating pattern of 0x41 ('A') — highly compressible */
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,
    0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41
};

/* =========================================================================
 * Unity lifecycle
 * ========================================================================= */

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Training: argument validation
 * ========================================================================= */

void test_train_null_out(void) {
    const uint8_t *pkts[] = { PKT_A };
    size_t         szs[]  = { sizeof(PKT_A) };
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_train(pkts, szs, 1, 1, NULL));
}

void test_train_reserved_model_id_zero(void) {
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_train(NULL, NULL, 0, 0, &d));
    TEST_ASSERT_NULL(d);
}

void test_train_reserved_model_id_255(void) {
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_train(NULL, NULL, 0, 255, &d));
    TEST_ASSERT_NULL(d);
}

void test_train_null_packets_with_count(void) {
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_train(NULL, NULL, 5, 1, &d));
    TEST_ASSERT_NULL(d);
}

void test_train_null_sizes_with_count(void) {
    netc_dict_t *d = NULL;
    const uint8_t *pkts[] = { PKT_A };
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_train(pkts, NULL, 1, 1, &d));
    TEST_ASSERT_NULL(d);
}

/* =========================================================================
 * Training: success paths
 * ========================================================================= */

void test_train_zero_packets(void) {
    /* Zero packets → uniform distribution, tables still valid */
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(NULL, NULL, 0, 42, &d));
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(42, netc_dict_model_id(d));
    netc_dict_free(d);
}

void test_train_single_packet(void) {
    netc_dict_t *d = NULL;
    const uint8_t *pkts[] = { PKT_A };
    size_t         szs[]  = { sizeof(PKT_A) };
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(pkts, szs, 1, 7, &d));
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(7, netc_dict_model_id(d));
    netc_dict_free(d);
}

void test_train_multiple_packets(void) {
    netc_dict_t *d = NULL;
    const uint8_t *pkts[] = { PKT_A, PKT_B, PKT_C };
    size_t         szs[]  = { sizeof(PKT_A), sizeof(PKT_B), sizeof(PKT_C) };
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(pkts, szs, 3, 100, &d));
    TEST_ASSERT_NOT_NULL(d);
    netc_dict_free(d);
}

/* =========================================================================
 * Normalization: frequency tables sum to TABLE_SIZE
 *
 * We verify this via save→load: the load path calls netc_tans_build()
 * which validates sum == TABLE_SIZE internally and returns -1 on failure.
 * A successful load implies all tables have valid frequency sums.
 * ========================================================================= */

void test_train_freq_tables_sum_to_table_size(void) {
    netc_dict_t *d = NULL;
    const uint8_t *pkts[] = { PKT_A, PKT_B };
    size_t         szs[]  = { sizeof(PKT_A), sizeof(PKT_B) };
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(pkts, szs, 2, 5, &d));
    TEST_ASSERT_NOT_NULL(d);

    /* Save and reload — load validates sum via netc_tans_build */
    void *blob = NULL;
    size_t blob_size = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_save(d, &blob, &blob_size));
    netc_dict_free(d);

    netc_dict_t *d2 = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_load(blob, blob_size, &d2));
    TEST_ASSERT_NOT_NULL(d2);

    netc_dict_free(d2);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * Serialization: save
 * ========================================================================= */

void test_save_null_dict(void) {
    void *blob = NULL;
    size_t sz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_save(NULL, &blob, &sz));
}

void test_save_null_out(void) {
    netc_dict_t *d = NULL;
    netc_dict_train(NULL, NULL, 0, 1, &d);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_save(d, NULL, NULL));
    netc_dict_free(d);
}

void test_save_blob_size(void) {
    netc_dict_t *d = NULL;
    netc_dict_train(NULL, NULL, 0, 3, &d);

    void *blob = NULL;
    size_t sz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_save(d, &blob, &sz));
    TEST_ASSERT_NOT_NULL(blob);
    /* v4 with LZP table */
    TEST_ASSERT_EQUAL_UINT(EXPECTED_BLOB_SIZE_V4, sz);

    netc_dict_free(d);
    netc_dict_free_blob(blob);
}

void test_save_magic_in_blob(void) {
    netc_dict_t *d = NULL;
    netc_dict_train(NULL, NULL, 0, 10, &d);

    void *blob = NULL;
    size_t sz = 0;
    netc_dict_save(d, &blob, &sz);

    const uint8_t *b = (const uint8_t *)blob;
    /* Magic "NETC" LE = 0x4E455443 */
    uint32_t magic = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    TEST_ASSERT_EQUAL_UINT32(0x4E455443U, magic);
    TEST_ASSERT_EQUAL_UINT8(10, b[5]);  /* model_id at offset 5 */

    netc_dict_free(d);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * Deserialization: load
 * ========================================================================= */

void test_load_null_data(void) {
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_load(NULL, 100, &d));
}

void test_load_null_out(void) {
    uint8_t *buf = (uint8_t *)calloc(1, EXPECTED_BLOB_SIZE_V4);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,
        netc_dict_load(buf, EXPECTED_BLOB_SIZE_V4, NULL));
    free(buf);
}

void test_load_short_blob(void) {
    uint8_t buf[4] = { 0x43, 0x54, 0x45, 0x4E }; /* magic "NETC" */
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID,
        netc_dict_load(buf, sizeof(buf), &d));
    TEST_ASSERT_NULL(d);
}

void test_load_wrong_magic(void) {
    uint8_t *buf = (uint8_t *)calloc(1, EXPECTED_BLOB_SIZE_V4);
    TEST_ASSERT_NOT_NULL(buf);
    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID,
        netc_dict_load(buf, EXPECTED_BLOB_SIZE_V4, &d));
    TEST_ASSERT_NULL(d);
    free(buf);
}

void test_load_wrong_version(void) {
    /* Build a valid blob then corrupt the version byte */
    netc_dict_t *src = NULL;
    netc_dict_train(NULL, NULL, 0, 1, &src);
    void *blob = NULL;
    size_t sz = 0;
    netc_dict_save(src, &blob, &sz);
    netc_dict_free(src);

    ((uint8_t *)blob)[4] = 0x99;  /* corrupt version (v3 and v4 accepted, 0x99 is not) */

    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_VERSION,
        netc_dict_load(blob, sz, &d));
    TEST_ASSERT_NULL(d);
    netc_dict_free_blob(blob);
}

void test_load_corrupt_checksum(void) {
    netc_dict_t *src = NULL;
    netc_dict_train(NULL, NULL, 0, 1, &src);
    void *blob = NULL;
    size_t sz = 0;
    netc_dict_save(src, &blob, &sz);
    netc_dict_free(src);

    /* Flip a bit in the checksum (last 4 bytes) */
    uint8_t *b = (uint8_t *)blob;
    b[sz - 1] ^= 0x01;

    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID,
        netc_dict_load(blob, sz, &d));
    TEST_ASSERT_NULL(d);
    netc_dict_free_blob(blob);
}

void test_load_corrupt_payload(void) {
    netc_dict_t *src = NULL;
    netc_dict_train(NULL, NULL, 0, 1, &src);
    void *blob = NULL;
    size_t sz = 0;
    netc_dict_save(src, &blob, &sz);
    netc_dict_free(src);

    /* Corrupt a frequency table byte (offset 8 = first freq entry) */
    uint8_t *b = (uint8_t *)blob;
    b[8] ^= 0xFF;

    /* Checksum will now mismatch → NETC_ERR_DICT_INVALID */
    netc_dict_t *d = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID,
        netc_dict_load(blob, sz, &d));
    TEST_ASSERT_NULL(d);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * Round-trip: train → save → load
 * ========================================================================= */

void test_roundtrip_empty_training(void) {
    netc_dict_t *src = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(NULL, NULL, 0, 77, &src));

    void *blob = NULL;
    size_t sz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_save(src, &blob, &sz));

    netc_dict_t *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_load(blob, sz, &loaded));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_UINT8(77, netc_dict_model_id(loaded));

    netc_dict_free(src);
    netc_dict_free(loaded);
    netc_dict_free_blob(blob);
}

void test_roundtrip_with_training_data(void) {
    netc_dict_t *src = NULL;
    const uint8_t *pkts[] = { PKT_A, PKT_B, PKT_C };
    size_t         szs[]  = { sizeof(PKT_A), sizeof(PKT_B), sizeof(PKT_C) };
    TEST_ASSERT_EQUAL_INT(NETC_OK,
        netc_dict_train(pkts, szs, 3, 55, &src));

    void *blob = NULL;
    size_t sz = 0;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_save(src, &blob, &sz));
    TEST_ASSERT_EQUAL_UINT(EXPECTED_BLOB_SIZE_V4, sz);

    netc_dict_t *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(NETC_OK, netc_dict_load(blob, sz, &loaded));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_UINT8(55, netc_dict_model_id(loaded));

    netc_dict_free(src);
    netc_dict_free(loaded);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * model_id accessor
 * ========================================================================= */

void test_model_id_null_dict(void) {
    TEST_ASSERT_EQUAL_UINT8(0, netc_dict_model_id(NULL));
}

void test_model_id_valid_dict(void) {
    netc_dict_t *d = NULL;
    netc_dict_train(NULL, NULL, 0, 123, &d);
    TEST_ASSERT_EQUAL_UINT8(123, netc_dict_model_id(d));
    netc_dict_free(d);
}

void test_model_id_all_valid_values(void) {
    /* Spot-check several valid model_ids (1–254) */
    for (int id = 1; id <= 10; id++) {
        netc_dict_t *d = NULL;
        TEST_ASSERT_EQUAL_INT(NETC_OK,
            netc_dict_train(NULL, NULL, 0, (uint8_t)id, &d));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)id, netc_dict_model_id(d));
        netc_dict_free(d);
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* Training: argument validation */
    RUN_TEST(test_train_null_out);
    RUN_TEST(test_train_reserved_model_id_zero);
    RUN_TEST(test_train_reserved_model_id_255);
    RUN_TEST(test_train_null_packets_with_count);
    RUN_TEST(test_train_null_sizes_with_count);

    /* Training: success paths */
    RUN_TEST(test_train_zero_packets);
    RUN_TEST(test_train_single_packet);
    RUN_TEST(test_train_multiple_packets);

    /* Normalization */
    RUN_TEST(test_train_freq_tables_sum_to_table_size);

    /* Serialization: save */
    RUN_TEST(test_save_null_dict);
    RUN_TEST(test_save_null_out);
    RUN_TEST(test_save_blob_size);
    RUN_TEST(test_save_magic_in_blob);

    /* Deserialization: load */
    RUN_TEST(test_load_null_data);
    RUN_TEST(test_load_null_out);
    RUN_TEST(test_load_short_blob);
    RUN_TEST(test_load_wrong_magic);
    RUN_TEST(test_load_wrong_version);
    RUN_TEST(test_load_corrupt_checksum);
    RUN_TEST(test_load_corrupt_payload);

    /* Round-trip */
    RUN_TEST(test_roundtrip_empty_training);
    RUN_TEST(test_roundtrip_with_training_data);

    /* model_id accessor */
    RUN_TEST(test_model_id_null_dict);
    RUN_TEST(test_model_id_valid_dict);
    RUN_TEST(test_model_id_all_valid_values);

    return UNITY_END();
}
