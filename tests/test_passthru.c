/**
 * test_passthru.c — Passthrough compression round-trip tests.
 *
 * Tests the Phase 1 passthrough baseline:
 *   - All data compresses to NETC_PKT_FLAG_PASSTHRU output in Phase 1
 *   - Decompression recovers the original bytes exactly
 *   - Header fields are correctly written and read
 *   - Passthrough guarantee: output ≤ src_size + NETC_MAX_OVERHEAD
 *   - Stats accumulate correctly when NETC_CFG_FLAG_STATS is set
 *   - Context sequence counter increments correctly
 *   - Stateless path produces equivalent output
 */

#include "unity.h"
#include "netc.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* =========================================================================
 * Fixtures
 * ========================================================================= */

static netc_dict_t *g_dict = NULL;
static netc_ctx_t  *g_ctx  = NULL;

static const uint8_t SAMPLE[64] = {
    0x01, 0x00, 0x00, 0x00,  /* sequence number */
    0x42, 0x00, 0x00, 0x00,  /* message type, flags, pad */
    0x3F, 0x80, 0x00, 0x00,  /* position.x = 1.0f */
    0x00, 0x00, 0x00, 0x00,  /* position.y = 0.0f */
    0x00, 0x00, 0x80, 0x3F,  /* position.z = 1.0f */
    0x00, 0x00, 0x00, 0x00,  /* velocity.x */
    0x00, 0x00, 0x00, 0x00,  /* velocity.y */
    0x00, 0x00, 0x00, 0x00,  /* velocity.z */
    0x00, 0x00, 0x80, 0x3F,  /* rotation.w */
    0x00, 0x00, 0x00, 0x00,  /* rotation.x */
    0x00, 0x00, 0x00, 0x00,  /* rotation.y */
    0x00, 0x00, 0x00, 0x00,  /* rotation.z */
    0x64, 0x00, 0x00, 0x00,  /* health, ammo, pad */
    0x01, 0x00, 0x00, 0x00,  /* entity_id */
    0x00, 0x00, 0x00, 0x00,  /* team_id, pad */
    0xAB, 0xCD, 0x00, 0x00   /* checksum, pad */
};

void setUp(void) {
    const uint8_t *packets[1] = { SAMPLE };
    size_t sizes[1] = { sizeof(SAMPLE) };
    netc_result_t r = netc_dict_train(packets, sizes, 1, 1, &g_dict);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

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
 * Helper: compress + decompress, verify round-trip
 * ========================================================================= */

static void assert_roundtrip(netc_ctx_t *ctx, const uint8_t *src, size_t src_len) {
    size_t  dst_cap = netc_compress_bound(src_len);
    uint8_t *dst    = (uint8_t *)malloc(dst_cap);
    uint8_t *rec    = (uint8_t *)malloc(src_len + 1);  /* +1 to detect overwrite */
    TEST_ASSERT_NOT_NULL(dst);
    TEST_ASSERT_NOT_NULL(rec);

    /* Compress */
    size_t        compressed_size = 0;
    netc_result_t r = netc_compress(ctx, src, src_len, dst, dst_cap, &compressed_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_GREATER_THAN(0, (int)compressed_size);

    /* Passthrough guarantee: output ≤ src_len + NETC_MAX_OVERHEAD */
    TEST_ASSERT_LESS_OR_EQUAL_UINT(src_len + NETC_MAX_OVERHEAD, compressed_size);

    /* Decompress */
    size_t decompressed_size = 0;
    r = netc_decompress(ctx, dst, compressed_size, rec, src_len, &decompressed_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    /* Exact byte-for-byte match */
    TEST_ASSERT_EQUAL_UINT(src_len, decompressed_size);
    TEST_ASSERT_EQUAL_MEMORY(src, rec, src_len);

    free(dst);
    free(rec);
}

/* =========================================================================
 * Round-trip tests for various packet sizes
 * ========================================================================= */

void test_roundtrip_8bytes(void) {
    uint8_t pkt[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_16bytes(void) {
    uint8_t pkt[16];
    for (int i = 0; i < 16; i++) pkt[i] = (uint8_t)i;
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_32bytes(void) {
    uint8_t pkt[32];
    for (int i = 0; i < 32; i++) pkt[i] = (uint8_t)(i * 7 + 3);
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_64bytes_game_state(void) {
    assert_roundtrip(g_ctx, SAMPLE, sizeof(SAMPLE));
}

void test_roundtrip_128bytes(void) {
    uint8_t pkt[128];
    for (int i = 0; i < 128; i++) pkt[i] = (uint8_t)i;
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_256bytes(void) {
    uint8_t pkt[256];
    for (int i = 0; i < 256; i++) pkt[i] = (uint8_t)i;
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_512bytes(void) {
    uint8_t pkt[512];
    for (int i = 0; i < 512; i++) pkt[i] = (uint8_t)(i ^ (i >> 8));
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_1500bytes_mtu(void) {
    uint8_t pkt[1500];
    for (int i = 0; i < 1500; i++) pkt[i] = (uint8_t)(i * 3);
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_max_packet(void) {
    /* Allocate on heap — 65535 bytes on stack would overflow */
    uint8_t *pkt = (uint8_t *)malloc(NETC_MAX_PACKET_SIZE);
    TEST_ASSERT_NOT_NULL(pkt);
    for (size_t i = 0; i < NETC_MAX_PACKET_SIZE; i++) pkt[i] = (uint8_t)(i * 7);
    assert_roundtrip(g_ctx, pkt, NETC_MAX_PACKET_SIZE);
    free(pkt);
}

void test_roundtrip_all_zeros(void) {
    uint8_t pkt[128] = { 0 };
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_all_ones(void) {
    uint8_t pkt[128];
    memset(pkt, 0xFF, sizeof(pkt));
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

void test_roundtrip_high_entropy(void) {
    /* Pseudo-random via LCG — simulates encrypted/random payload */
    uint8_t pkt[128];
    uint32_t state = 0xDEADBEEFU;
    for (int i = 0; i < 128; i++) {
        state = state * 1664525u + 1013904223u;
        pkt[i] = (uint8_t)(state >> 24);
    }
    assert_roundtrip(g_ctx, pkt, sizeof(pkt));
}

/* =========================================================================
 * Header field validation
 * ========================================================================= */

void test_header_passthru_flag_set(void) {
    uint8_t src[32];
    memset(src, 0xAA, sizeof(src));
    uint8_t dst[64];
    size_t  out = 0;

    netc_result_t r = netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    /* Header byte [4] = flags */
    TEST_ASSERT_BITS(NETC_PKT_FLAG_PASSTHRU, NETC_PKT_FLAG_PASSTHRU, dst[4]);
}

void test_header_algorithm_passthru(void) {
    uint8_t src[32];
    memset(src, 0xBB, sizeof(src));
    uint8_t dst[64];
    size_t  out = 0;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Header byte [5] = algorithm */
    TEST_ASSERT_EQUAL_UINT8(NETC_ALG_PASSTHRU, dst[5]);
}

void test_header_model_id_matches_dict(void) {
    uint8_t src[32];
    memset(src, 0xCC, sizeof(src));
    uint8_t dst[64];
    size_t  out = 0;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Header byte [6] = model_id */
    TEST_ASSERT_EQUAL_UINT8(netc_dict_model_id(g_dict), dst[6]);
}

void test_header_original_size_correct(void) {
    uint8_t src[42];
    memset(src, 0x55, sizeof(src));
    uint8_t dst[64];
    size_t  out = 0;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Header bytes [0..1] = original_size (LE) */
    uint16_t orig = (uint16_t)(dst[0] | ((uint16_t)dst[1] << 8));
    TEST_ASSERT_EQUAL_UINT16(42, orig);
}

void test_header_compressed_size_equals_original_on_passthru(void) {
    /* Use non-repetitive ascending bytes — RLE cannot compress these,
     * and tANS will likely not compress data unseen in training either,
     * so this exercises the raw passthrough path. */
    uint8_t src[37];
    for (int i = 0; i < 37; i++) src[i] = (uint8_t)(i ^ 0xAA ^ (i * 7));
    uint8_t dst[64];
    size_t  out = 0;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Compressed output must be ≤ original + header (AD-006) */
    uint16_t orig = (uint16_t)(dst[0] | ((uint16_t)dst[1] << 8));
    uint16_t comp = (uint16_t)(dst[2] | ((uint16_t)dst[3] << 8));
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(orig, comp);
}

/* =========================================================================
 * Output size guarantee
 * ========================================================================= */

void test_output_size_equals_src_plus_header(void) {
    /* Use non-repetitive data that won't compress (ascending non-training bytes).
     * Output must be ≤ src + header (AD-006 passthrough guarantee). */
    uint8_t src[100];
    for (int i = 0; i < 100; i++) src[i] = (uint8_t)(i ^ 0x55 ^ (i * 13));
    uint8_t dst[200];
    size_t  out = 0;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    TEST_ASSERT_LESS_OR_EQUAL_UINT(sizeof(src) + NETC_HEADER_SIZE, out);
}

void test_output_never_exceeds_bound(void) {
    static const size_t sizes[] = { 1, 8, 16, 32, 64, 128, 256, 512, 1024, 1500 };
    uint8_t src[1500];
    memset(src, 0xAB, sizeof(src));

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t src_len = sizes[s];
        size_t cap     = netc_compress_bound(src_len);
        uint8_t *dst   = (uint8_t *)malloc(cap);
        TEST_ASSERT_NOT_NULL(dst);

        size_t out = 0;
        netc_result_t r = netc_compress(g_ctx, src, src_len, dst, cap, &out);
        TEST_ASSERT_EQUAL_INT(NETC_OK, r);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(cap, out);

        free(dst);
    }
}

/* =========================================================================
 * Context sequence counter
 * ========================================================================= */

void test_context_seq_increments(void) {
    uint8_t src[32];
    memset(src, 0xDE, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    /* First packet: seq should be 0 */
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);

    /* Second packet: seq should be 1 */
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(1, dst[7]);
}

void test_context_seq_wraps_at_255(void) {
    uint8_t src[32];
    memset(src, 0xCA, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    /* seq starts at 0 and increments after emission.
     * 256 calls produce seq values 0..255. The 256th call emits seq=255.
     * The 257th call wraps and emits seq=0. */
    for (int i = 0; i < 256; i++) {
        netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    }
    TEST_ASSERT_EQUAL_UINT8(255, dst[7]);

    /* Next must wrap to 0 */
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);
}

void test_context_seq_reset_after_ctx_reset(void) {
    uint8_t src[32];
    memset(src, 0xFE, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    /* Advance seq */
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Reset and verify seq restarts */
    netc_ctx_reset(g_ctx);
    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);
}

/* =========================================================================
 * Statistics accumulation
 * ========================================================================= */

void test_stats_accumulate_correctly(void) {
    netc_cfg_t cfg = {
        .flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_STATS,
    };
    netc_ctx_t *ctx = netc_ctx_create(g_dict, &cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    uint8_t src[64];
    memset(src, 0x11, sizeof(src));
    uint8_t dst[128];
    size_t  out;

    for (int i = 0; i < 5; i++) {
        netc_compress(ctx, src, sizeof(src), dst, sizeof(dst), &out);
    }

    netc_stats_t stats;
    netc_result_t r = netc_ctx_stats(ctx, &stats);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT64(5, stats.packets_compressed);
    TEST_ASSERT_EQUAL_UINT64(5 * sizeof(src), stats.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(5, stats.passthrough_count);

    netc_ctx_destroy(ctx);
}

/* =========================================================================
 * Stateless path round-trip
 * ========================================================================= */

void test_stateless_roundtrip(void) {
    uint8_t src[64];
    memset(src, 0x99, sizeof(src));

    size_t  cap = netc_compress_bound(sizeof(src));
    uint8_t dst[200];
    uint8_t rec[64];
    size_t  comp_size = 0, decomp_size = 0;

    netc_result_t r = netc_compress_stateless(g_dict, src, sizeof(src),
                                              dst, cap, &comp_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    r = netc_decompress_stateless(g_dict, dst, comp_size,
                                  rec, sizeof(rec), &decomp_size);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_UINT(sizeof(src), decomp_size);
    TEST_ASSERT_EQUAL_MEMORY(src, rec, sizeof(src));
}

void test_stateless_context_seq_always_zero(void) {
    uint8_t src[32];
    memset(src, 0x44, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    /* Stateless always emits context_seq = 0 (no persistent state) */
    netc_compress_stateless(g_dict, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);

    netc_compress_stateless(g_dict, src, sizeof(src), dst, sizeof(dst), &out);
    TEST_ASSERT_EQUAL_UINT8(0, dst[7]);
}

/* =========================================================================
 * Decompression rejects wrong model_id
 * ========================================================================= */

void test_decompress_rejects_wrong_model_id(void) {
    uint8_t src[32];
    memset(src, 0x55, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Patch model_id in header to a wrong value */
    dst[6] = 0x7F;  /* != g_dict->model_id (1) */
    /* Also clear the passthru flag to force model_id check */
    dst[4] = dst[4] & (uint8_t)0xFBu;  /* clear NETC_PKT_FLAG_PASSTHRU (0x04) */
    dst[5] = NETC_ALG_TANS;  /* pretend it's a real compressed packet */

    uint8_t rec[64];
    size_t  rec_size;
    netc_result_t r = netc_decompress(g_ctx, dst, out, rec, sizeof(rec), &rec_size);
    /* model_id mismatch is checked before algorithm dispatch: NETC_ERR_VERSION */
    TEST_ASSERT_EQUAL_INT(NETC_ERR_VERSION, r);
}

void test_decompress_corrupt_algorithm_byte(void) {
    uint8_t src[32];
    memset(src, 0x66, sizeof(src));
    uint8_t dst[64];
    size_t  out;

    netc_compress(g_ctx, src, sizeof(src), dst, sizeof(dst), &out);

    /* Clear PASSTHRU flag and set an unknown algorithm */
    dst[4] = 0x00;   /* clear flags */
    dst[5] = 0x42;   /* unknown algorithm */

    uint8_t rec[64];
    size_t  rec_size;
    netc_result_t r = netc_decompress(g_ctx, dst, out, rec, sizeof(rec), &rec_size);
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT, r);
}

/* =========================================================================
 * Minimum packet size: single byte
 * ========================================================================= */

void test_roundtrip_single_byte(void) {
    uint8_t src[1] = { (uint8_t)0xAA };
    assert_roundtrip(g_ctx, src, sizeof(src));
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* Round-trip by size */
    RUN_TEST(test_roundtrip_single_byte);
    RUN_TEST(test_roundtrip_8bytes);
    RUN_TEST(test_roundtrip_16bytes);
    RUN_TEST(test_roundtrip_32bytes);
    RUN_TEST(test_roundtrip_64bytes_game_state);
    RUN_TEST(test_roundtrip_128bytes);
    RUN_TEST(test_roundtrip_256bytes);
    RUN_TEST(test_roundtrip_512bytes);
    RUN_TEST(test_roundtrip_1500bytes_mtu);
    RUN_TEST(test_roundtrip_max_packet);
    RUN_TEST(test_roundtrip_all_zeros);
    RUN_TEST(test_roundtrip_all_ones);
    RUN_TEST(test_roundtrip_high_entropy);

    /* Header fields */
    RUN_TEST(test_header_passthru_flag_set);
    RUN_TEST(test_header_algorithm_passthru);
    RUN_TEST(test_header_model_id_matches_dict);
    RUN_TEST(test_header_original_size_correct);
    RUN_TEST(test_header_compressed_size_equals_original_on_passthru);

    /* Output size guarantee */
    RUN_TEST(test_output_size_equals_src_plus_header);
    RUN_TEST(test_output_never_exceeds_bound);

    /* Sequence counter */
    RUN_TEST(test_context_seq_increments);
    RUN_TEST(test_context_seq_wraps_at_255);
    RUN_TEST(test_context_seq_reset_after_ctx_reset);

    /* Statistics */
    RUN_TEST(test_stats_accumulate_correctly);

    /* Stateless path */
    RUN_TEST(test_stateless_roundtrip);
    RUN_TEST(test_stateless_context_seq_always_zero);

    /* Decompressor security */
    RUN_TEST(test_decompress_rejects_wrong_model_id);
    RUN_TEST(test_decompress_corrupt_algorithm_byte);

    return UNITY_END();
}
