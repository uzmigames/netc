/**
 * test_api.c — API contract tests.
 *
 * Tests all public API functions for correct return codes, NULL safety,
 * boundary conditions, and error paths.
 *
 * Coverage target: all netc_result_t codes, all NULL/invalid argument paths,
 * context lifecycle, dictionary lifecycle.
 */

#include "unity.h"
#include "netc.h"
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Test fixtures
 * ========================================================================= */

static netc_dict_t  *g_dict = NULL;
static netc_ctx_t   *g_ctx  = NULL;

/* Representative 64-byte packet for training */
static const uint8_t SAMPLE_PACKET[64] = {
    0x01, 0x00, 0x00, 0x00,  /* sequence number */
    0x42, 0x00,              /* message type, flags */
    0x3F, 0x80, 0x00, 0x00,  /* position.x (1.0f) */
    0x00, 0x00, 0x00, 0x00,  /* position.y (0.0f) */
    0x00, 0x00, 0x80, 0x3F,  /* position.z (1.0f) */
    0x00, 0x00, 0x00, 0x00,  /* velocity.x */
    0x00, 0x00, 0x00, 0x00,  /* velocity.y */
    0x00, 0x00, 0x00, 0x00,  /* velocity.z */
    0x00, 0x00, 0x80, 0x3F,  /* rotation.w */
    0x00, 0x00, 0x00, 0x00,  /* rotation.x */
    0x00, 0x00, 0x00, 0x00,  /* rotation.y */
    0x00, 0x00, 0x00, 0x00,  /* rotation.z */
    0x64, 0x00,              /* health (100) */
    0x00, 0x00,              /* ammo */
    0x01, 0x00, 0x00, 0x00,  /* entity_id */
    0x00, 0x00, 0x00, 0x00,  /* team_id, padding */
    0xAB, 0xCD,              /* checksum */
};

void setUp(void) {
    /* Create a trained dictionary before each test group */
    const uint8_t *packets[1] = { SAMPLE_PACKET };
    size_t sizes[1] = { sizeof(SAMPLE_PACKET) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 1, &g_dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(g_dict);

    g_ctx = netc_ctx_create(g_dict, NULL);
    TEST_ASSERT_NOT_NULL(g_ctx);
}

void tearDown(void) {
    netc_ctx_destroy(g_ctx);
    g_ctx = NULL;
    netc_dict_free(g_dict);
    g_dict = NULL;
}

/* =========================================================================
 * netc_version
 * ========================================================================= */

void test_version_not_null(void) {
    const char *v = netc_version();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(v));
}

void test_version_format(void) {
    const char *v = netc_version();
    /* Must contain at least two dots: MAJOR.MINOR.PATCH */
    int dots = 0;
    for (const char *p = v; *p; p++) {
        if (*p == '.') dots++;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(2, dots);
}

/* =========================================================================
 * netc_strerror
 * ========================================================================= */

void test_strerror_ok(void) {
    TEST_ASSERT_NOT_NULL(netc_strerror(NETC_OK));
}

void test_strerror_all_codes(void) {
    static const netc_result_t codes[] = {
        NETC_OK,
        NETC_ERR_NOMEM,
        NETC_ERR_TOOBIG,
        NETC_ERR_CORRUPT,
        NETC_ERR_DICT_INVALID,
        NETC_ERR_BUF_SMALL,
        NETC_ERR_CTX_NULL,
        NETC_ERR_UNSUPPORTED,
        NETC_ERR_VERSION,
        NETC_ERR_INVALID_ARG,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *msg = netc_strerror(codes[i]);
        TEST_ASSERT_NOT_NULL(msg);
        TEST_ASSERT_GREATER_THAN(0, (int)strlen(msg));
    }
}

void test_strerror_unknown_code(void) {
    /* Unknown code must not crash and must return a non-NULL string */
    const char *msg = netc_strerror((netc_result_t)-999);
    TEST_ASSERT_NOT_NULL(msg);
}

/* =========================================================================
 * netc_ctx_create
 * ========================================================================= */

void test_ctx_create_null_dict_succeeds(void) {
    /* NULL dict is valid — passthrough-only mode */
    netc_ctx_t *ctx = netc_ctx_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    netc_ctx_destroy(ctx);
}

void test_ctx_create_null_cfg_uses_defaults(void) {
    netc_ctx_t *ctx = netc_ctx_create(g_dict, NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    netc_ctx_destroy(ctx);
}

void test_ctx_create_custom_cfg(void) {
    netc_cfg_t cfg = {
        .flags             = NETC_CFG_FLAG_STATELESS,
        .ring_buffer_size  = 0,
        .compression_level = 1,
        .simd_level        = 1,
        .arena_size        = 8192,
    };
    netc_ctx_t *ctx = netc_ctx_create(g_dict, &cfg);
    TEST_ASSERT_NOT_NULL(ctx);
    netc_ctx_destroy(ctx);
}

/* =========================================================================
 * netc_ctx_destroy
 * ========================================================================= */

void test_ctx_destroy_null_is_safe(void) {
    /* Must not crash */
    netc_ctx_destroy(NULL);
}

/* =========================================================================
 * netc_ctx_reset
 * ========================================================================= */

void test_ctx_reset_null_is_safe(void) {
    netc_ctx_reset(NULL);
}

void test_ctx_reset_valid_ctx(void) {
    netc_ctx_reset(g_ctx);
    /* After reset, compress must still work */
    uint8_t src[32] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t dst[64];
    size_t dst_size = 0;
    netc_result_t r = netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &dst_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_GREATER_THAN(0, (int)dst_size);
}

/* =========================================================================
 * netc_ctx_stats
 * ========================================================================= */

void test_ctx_stats_without_flag_returns_unsupported(void) {
    netc_stats_t stats;
    /* g_ctx was created without NETC_CFG_FLAG_STATS */
    netc_result_t r = netc_ctx_stats(g_ctx, &stats);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_UNSUPPORTED, r);
}

void test_ctx_stats_null_ctx(void) {
    netc_stats_t stats;
    netc_result_t r = netc_ctx_stats(NULL, &stats);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL, r);
}

void test_ctx_stats_null_out(void) {
    netc_result_t r = netc_ctx_stats(g_ctx, NULL);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_ctx_stats_with_flag(void) {
    netc_cfg_t cfg = { .flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_STATS };
    netc_ctx_t *ctx = netc_ctx_create(g_dict, &cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    netc_stats_t stats;
    netc_result_t r = netc_ctx_stats(ctx, &stats);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT64(0, stats.packets_compressed);

    netc_ctx_destroy(ctx);
}

/* =========================================================================
 * netc_dict_train
 * ========================================================================= */

void test_dict_train_basic(void) {
    netc_dict_t *dict = NULL;
    const uint8_t *packets[1] = { SAMPLE_PACKET };
    size_t sizes[1] = { sizeof(SAMPLE_PACKET) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(dict);
    TEST_ASSERT_EQUAL_UINT8(1, netc_dict_model_id(dict));
    netc_dict_free(dict);
}

void test_dict_train_null_out(void) {
    const uint8_t *packets[1] = { SAMPLE_PACKET };
    size_t sizes[1] = { sizeof(SAMPLE_PACKET) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 1, NULL);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_dict_train_reserved_model_id_0(void) {
    netc_dict_t *dict = NULL;
    const uint8_t *packets[1] = { SAMPLE_PACKET };
    size_t sizes[1] = { sizeof(SAMPLE_PACKET) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 0, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
    TEST_ASSERT_NULL(dict);
}

void test_dict_train_reserved_model_id_255(void) {
    netc_dict_t *dict = NULL;
    const uint8_t *packets[1] = { SAMPLE_PACKET };
    size_t sizes[1] = { sizeof(SAMPLE_PACKET) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 255, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
    TEST_ASSERT_NULL(dict);
}

void test_dict_train_zero_packets(void) {
    /* count=0 with NULL pointers is valid (empty corpus) */
    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(NULL, NULL, 0, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(dict);
    netc_dict_free(dict);
}

void test_dict_train_null_packets_nonzero_count(void) {
    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_train(NULL, NULL, 5, 1, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
    TEST_ASSERT_NULL(dict);
}

/* =========================================================================
 * netc_dict_save / netc_dict_load
 * ========================================================================= */

void test_dict_save_load_roundtrip(void) {
    void   *blob      = NULL;
    size_t  blob_size = 0;
    netc_result_t r = netc_dict_save(g_dict, &blob, &blob_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(blob);
    TEST_ASSERT_GREATER_THAN(0, (int)blob_size);

    netc_dict_t *loaded = NULL;
    r = netc_dict_load(blob, blob_size, &loaded);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_UINT8(netc_dict_model_id(g_dict), netc_dict_model_id(loaded));

    netc_dict_free(loaded);
    netc_dict_free_blob(blob);
}

void test_dict_load_null_data(void) {
    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_load(NULL, 16, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
    TEST_ASSERT_NULL(dict);
}

void test_dict_load_null_out(void) {
    uint8_t buf[16] = { 0 };
    netc_result_t r = netc_dict_load(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_dict_load_too_small(void) {
    uint8_t buf[4] = { 0 };  /* too small for header */
    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_load(buf, sizeof(buf), &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, r);
    TEST_ASSERT_NULL(dict);
}

void test_dict_load_bad_magic(void) {
    /* Save a valid dict, corrupt the magic */
    void  *blob = NULL;
    size_t blob_size = 0;
    netc_dict_save(g_dict, &blob, &blob_size);
    ((uint8_t *)blob)[0] = 0xFF;  /* corrupt magic */

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_load(blob, blob_size, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, r);
    TEST_ASSERT_NULL(dict);
    netc_dict_free_blob(blob);
}

void test_dict_load_bad_checksum(void) {
    void  *blob = NULL;
    size_t blob_size = 0;
    netc_dict_save(g_dict, &blob, &blob_size);
    /* Corrupt the last byte of the checksum */
    ((uint8_t *)blob)[blob_size - 1] ^= 0xFF;

    netc_dict_t *dict = NULL;
    netc_result_t r = netc_dict_load(blob, blob_size, &dict);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, r);
    TEST_ASSERT_NULL(dict);
    netc_dict_free_blob(blob);
}

/* =========================================================================
 * netc_dict_free / netc_dict_free_blob / netc_dict_model_id
 * ========================================================================= */

void test_dict_free_null_is_safe(void) {
    netc_dict_free(NULL);
}

void test_dict_free_blob_null_is_safe(void) {
    netc_dict_free_blob(NULL);
}

void test_dict_model_id_null_returns_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, netc_dict_model_id(NULL));
}

/* =========================================================================
 * netc_compress — argument validation
 * ========================================================================= */

void test_compress_null_ctx(void) {
    uint8_t src[8] = { 0 };
    uint8_t dst[32];
    size_t  out = 0;
    netc_result_t r = netc_compress(NULL, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL, r);
    TEST_ASSERT_EQUAL_UINT(0, out);
}

void test_compress_null_src(void) {
    uint8_t dst[32];
    size_t  out = 0;
    netc_result_t r = netc_compress(g_ctx, NULL, 8, dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_compress_null_dst(void) {
    uint8_t src[8] = { 0 };
    size_t  out = 0;
    netc_result_t r = netc_compress(g_ctx, src, sizeof(src), NULL, 32, &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_compress_null_dst_size(void) {
    uint8_t src[8] = { 0 };
    uint8_t dst[32];
    netc_result_t r = netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), NULL);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_compress_input_too_large(void) {
    /* Simulate >65535 bytes by passing an oversized src_size value */
    uint8_t src[8] = { 0 };
    uint8_t dst[32];
    size_t  out = 0;
    netc_result_t r = netc_compress(g_ctx, src, NETC_MAX_PACKET_SIZE + 1, dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_TOOBIG, r);
}

void test_compress_dst_too_small(void) {
    uint8_t src[16] = { 0x01, 0x02 };
    uint8_t dst[4];   /* smaller than NETC_HEADER_SIZE */
    size_t  out = 0;
    netc_result_t r = netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL, r);
}

/* =========================================================================
 * netc_decompress — argument validation
 * ========================================================================= */

void test_decompress_null_ctx(void) {
    uint8_t src[16] = { 0 };
    uint8_t dst[16];
    size_t  out = 0;
    netc_result_t r = netc_decompress(NULL, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL, r);
}

void test_decompress_null_src(void) {
    uint8_t dst[16];
    size_t  out = 0;
    netc_result_t r = netc_decompress(g_ctx, NULL, 16, dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_decompress_null_dst(void) {
    uint8_t src[16] = { 0 };
    size_t  out = 0;
    netc_result_t r = netc_decompress(g_ctx, src, sizeof(src), NULL, 16, &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_decompress_null_dst_size(void) {
    uint8_t src[16] = { 0 };
    uint8_t dst[16];
    netc_result_t r = netc_decompress(g_ctx, src, sizeof(src), dst, sizeof(dst), NULL);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_decompress_truncated_header(void) {
    uint8_t src[4] = { 0x00, 0x08, 0x00, 0x00 };  /* less than 8 bytes */
    uint8_t dst[64];
    size_t  out = 0;
    netc_result_t r = netc_decompress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, r);
}

void test_decompress_original_size_exceeds_max(void) {
    /* Craft a header with original_size = 65535 but dst_cap = 8 */
    uint8_t src[NETC_HEADER_SIZE] = {
        0xFF, 0xFF,  /* original_size = 65535 */
        0x00, 0x00,  /* compressed_size = 0 */
        NETC_PKT_FLAG_PASSTHRU, NETC_ALG_PASSTHRU, 0x01, 0x00
    };
    uint8_t dst[8];
    size_t  out = 0;
    netc_result_t r = netc_decompress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL, r);
}

/* =========================================================================
 * netc_compress_stateless — argument validation
 * ========================================================================= */

void test_compress_stateless_null_dict(void) {
    uint8_t src[8] = { 0 };
    uint8_t dst[32];
    size_t  out = 0;
    netc_result_t r = netc_compress_stateless(NULL, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

void test_compress_stateless_too_large(void) {
    uint8_t src[8] = { 0 };
    uint8_t dst[32];
    size_t  out = 0;
    netc_result_t r = netc_compress_stateless(g_dict, src, NETC_MAX_PACKET_SIZE + 1,
                                              dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_TOOBIG, r);
}

/* =========================================================================
 * netc_decompress_stateless — argument validation
 * ========================================================================= */

void test_decompress_stateless_null_dict(void) {
    uint8_t src[16] = { 0 };
    uint8_t dst[16];
    size_t  out = 0;
    netc_result_t r = netc_decompress_stateless(NULL, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG, r);
}

/* =========================================================================
 * netc_compress_bound
 * ========================================================================= */

void test_compress_bound(void) {
    TEST_ASSERT_EQUAL_UINT(NETC_MAX_OVERHEAD,       netc_compress_bound(0));
    TEST_ASSERT_EQUAL_UINT(128 + NETC_MAX_OVERHEAD, netc_compress_bound(128));
    TEST_ASSERT_EQUAL_UINT(NETC_MAX_PACKET_SIZE + NETC_MAX_OVERHEAD,
                           netc_compress_bound(NETC_MAX_PACKET_SIZE));
}

/* =========================================================================
 * Constants sanity checks
 * ========================================================================= */

void test_constants_header_size(void) {
    TEST_ASSERT_EQUAL_UINT(8, NETC_HEADER_SIZE);
}

void test_constants_max_overhead(void) {
    TEST_ASSERT_EQUAL_UINT(8, NETC_MAX_OVERHEAD);
}

void test_constants_max_packet_size(void) {
    TEST_ASSERT_EQUAL_UINT(65535, NETC_MAX_PACKET_SIZE);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* Version and utility */
    RUN_TEST(test_version_not_null);
    RUN_TEST(test_version_format);
    RUN_TEST(test_strerror_ok);
    RUN_TEST(test_strerror_all_codes);
    RUN_TEST(test_strerror_unknown_code);
    RUN_TEST(test_compress_bound);
    RUN_TEST(test_constants_header_size);
    RUN_TEST(test_constants_max_overhead);
    RUN_TEST(test_constants_max_packet_size);

    /* Context lifecycle */
    RUN_TEST(test_ctx_create_null_dict_succeeds);
    RUN_TEST(test_ctx_create_null_cfg_uses_defaults);
    RUN_TEST(test_ctx_create_custom_cfg);
    RUN_TEST(test_ctx_destroy_null_is_safe);
    RUN_TEST(test_ctx_reset_null_is_safe);
    RUN_TEST(test_ctx_reset_valid_ctx);
    RUN_TEST(test_ctx_stats_without_flag_returns_unsupported);
    RUN_TEST(test_ctx_stats_null_ctx);
    RUN_TEST(test_ctx_stats_null_out);
    RUN_TEST(test_ctx_stats_with_flag);

    /* Dictionary */
    RUN_TEST(test_dict_train_basic);
    RUN_TEST(test_dict_train_null_out);
    RUN_TEST(test_dict_train_reserved_model_id_0);
    RUN_TEST(test_dict_train_reserved_model_id_255);
    RUN_TEST(test_dict_train_zero_packets);
    RUN_TEST(test_dict_train_null_packets_nonzero_count);
    RUN_TEST(test_dict_save_load_roundtrip);
    RUN_TEST(test_dict_load_null_data);
    RUN_TEST(test_dict_load_null_out);
    RUN_TEST(test_dict_load_too_small);
    RUN_TEST(test_dict_load_bad_magic);
    RUN_TEST(test_dict_load_bad_checksum);
    RUN_TEST(test_dict_free_null_is_safe);
    RUN_TEST(test_dict_free_blob_null_is_safe);
    RUN_TEST(test_dict_model_id_null_returns_zero);

    /* Compress argument validation */
    RUN_TEST(test_compress_null_ctx);
    RUN_TEST(test_compress_null_src);
    RUN_TEST(test_compress_null_dst);
    RUN_TEST(test_compress_null_dst_size);
    RUN_TEST(test_compress_input_too_large);
    RUN_TEST(test_compress_dst_too_small);

    /* Decompress argument validation */
    RUN_TEST(test_decompress_null_ctx);
    RUN_TEST(test_decompress_null_src);
    RUN_TEST(test_decompress_null_dst);
    RUN_TEST(test_decompress_null_dst_size);
    RUN_TEST(test_decompress_truncated_header);
    RUN_TEST(test_decompress_original_size_exceeds_max);

    /* Stateless argument validation */
    RUN_TEST(test_compress_stateless_null_dict);
    RUN_TEST(test_compress_stateless_too_large);
    RUN_TEST(test_decompress_stateless_null_dict);

    return UNITY_END();
}
