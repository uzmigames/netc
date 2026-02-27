/**
 * test_delta.c — Tests for Phase 3: field-class-aware delta prediction.
 *
 * Tests:
 *
 * ## 1. Delta encode/decode primitives (netc_delta.h)
 *   1.1 Round-trip: encode then decode recovers original bytes
 *   1.2 Field-class XOR regions (offsets 0-15, 64-255) use XOR
 *   1.3 Field-class SUB regions (offsets 16-63, 256+) use subtraction
 *   1.4 Zero prev → residual equals curr (identity predictor)
 *   1.5 Same prev and curr → residual is all zeros
 *   1.6 Wraparound: subtraction wraps at 256 (uint8_t)
 *   1.7 Decode in-place: curr == residual is allowed
 *   1.8 Large packet (>256 bytes): TAIL region uses SUB
 *
 * ## 2. Pipeline integration — delta + tANS round-trip (stateful)
 *   2.1 Two sequential packets with delta enabled: both round-trip correctly
 *   2.2 First packet (no prior history) compresses without delta flag
 *   2.3 Second packet has NETC_PKT_FLAG_DELTA set when delta is beneficial
 *   2.4 Delta improves ratio for correlated packets vs no-delta
 *   2.5 Multiple sequential packets all round-trip correctly
 *   2.6 ctx_reset clears delta history (next packet treats as first)
 *
 * ## 3. Pipeline integration — passthrough path with delta
 *   3.1 Passthrough packet updates delta predictor (next packet can delta)
 *   3.2 Size mismatch between consecutive packets: delta is skipped
 *
 * ## 4. Delta disabled
 *   4.1 NETC_PKT_FLAG_DELTA is never set when cfg flag is absent
 *   4.2 Compression without delta flag round-trips correctly
 *
 * ## 5. Spec scenarios (from delta/spec.md)
 *   5.1 delta_encode/delta_decode: residual[i] = (C[i]-P[i]) mod 256 for SUB region
 *   5.2 Delta disabled for small packets (< NETC_DELTA_MIN_SIZE)
 */

#include "unity.h"
#include "netc.h"
#include "algo/netc_delta.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* =========================================================================
 * Test fixtures
 * ========================================================================= */

/* Highly correlated game-state-like packets: slow-moving floats + counters */
static uint8_t s_pkt_base[512];   /* "previous" packet */
static uint8_t s_pkt_next[512];   /* "current" packet — small delta from base */

static netc_dict_t *s_dict = NULL;
static netc_ctx_t  *s_ctx  = NULL;
static netc_ctx_t  *s_ctx_nodelta = NULL;

/* Build training corpus of correlated packets */
static void build_training_data(const uint8_t **pkts_out, size_t *szs_out, int count) {
    for (int p = 0; p < count; p++) {
        uint8_t *pkt = (uint8_t *)pkts_out[p];
        /* Header region (0-15): flags and types — small enum values */
        for (int i = 0; i < 16; i++) pkt[i] = (uint8_t)(i < 4 ? 0x01 : 0x00);
        /* Subheader region (16-63): counters — incrementing values */
        for (int i = 16; i < 64; i++) pkt[i] = (uint8_t)(p + i);
        /* Body region (64-255): float-like bytes */
        for (int i = 64; i < 256 && i < (int)szs_out[p]; i++) {
            pkt[i] = (uint8_t)(0x40 + (i & 0x0F) + p);
        }
        /* Tail region (256+): bulk integers */
        for (int i = 256; i < (int)szs_out[p]; i++) {
            pkt[i] = (uint8_t)(i + p);
        }
    }
}

/* 8 training packets of the same size for dictionary training */
#define TRAIN_COUNT 8
#define PKT_SIZE    512

static uint8_t s_train_data[TRAIN_COUNT][PKT_SIZE];

void setUp(void) {
    /* Build base and next packets */
    memset(s_pkt_base, 0, sizeof(s_pkt_base));
    memset(s_pkt_next, 0, sizeof(s_pkt_next));

    /* Base: mixed field classes */
    for (int i = 0; i < (int)sizeof(s_pkt_base); i++) {
        s_pkt_base[i] = (uint8_t)(0x41 + (i & 0x0F));
    }
    /* Next: small delta — most bytes same, some incremented by 1 */
    memcpy(s_pkt_next, s_pkt_base, sizeof(s_pkt_base));
    for (int i = 0; i < (int)sizeof(s_pkt_next); i += 16) {
        s_pkt_next[i] = (uint8_t)(s_pkt_base[i] + 1);
    }

    /* Build training data */
    const uint8_t *pkt_ptrs[TRAIN_COUNT];
    size_t pkt_szs[TRAIN_COUNT];
    for (int i = 0; i < TRAIN_COUNT; i++) {
        pkt_ptrs[i] = s_train_data[i];
        pkt_szs[i]  = PKT_SIZE;
    }
    build_training_data(pkt_ptrs, pkt_szs, TRAIN_COUNT);

    netc_result_t r = netc_dict_train(pkt_ptrs, pkt_szs, TRAIN_COUNT, 2, &s_dict);
    (void)r;

    /* Context with delta enabled */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
    s_ctx = netc_ctx_create(s_dict, &cfg);

    /* Context without delta */
    netc_cfg_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    cfg2.flags = NETC_CFG_FLAG_STATEFUL;
    s_ctx_nodelta = netc_ctx_create(s_dict, &cfg2);
}

void tearDown(void) {
    netc_ctx_destroy(s_ctx);
    s_ctx = NULL;
    netc_ctx_destroy(s_ctx_nodelta);
    s_ctx_nodelta = NULL;
    netc_dict_free(s_dict);
    s_dict = NULL;
}

/* =========================================================================
 * Helper: check that two byte arrays are equal
 * ========================================================================= */
static void assert_bytes_equal(const uint8_t *a, const uint8_t *b,
                               size_t n, const char *msg) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s: mismatch at offset %zu: got 0x%02X want 0x%02X",
                     msg, i, a[i], b[i]);
            TEST_FAIL_MESSAGE(buf);
            return;
        }
    }
}

/* =========================================================================
 * 1. Delta encode/decode primitives
 * ========================================================================= */

void test_delta_roundtrip_small(void) {
    /* 1.1 Round-trip: encode then decode recovers original bytes (small packet) */
    uint8_t prev[32], curr[32], residual[32], recovered[32];

    for (int i = 0; i < 32; i++) {
        prev[i] = (uint8_t)(i * 3);
        curr[i] = (uint8_t)(i * 3 + 7);
    }

    netc_delta_encode(prev, curr, residual, 32);
    netc_delta_decode(prev, residual, recovered, 32);

    assert_bytes_equal(curr, recovered, 32, "delta roundtrip small");
}

void test_delta_roundtrip_large(void) {
    /* 1.1 (extended) Round-trip across all field-class regions */
    uint8_t prev[512], curr[512], residual[512], recovered[512];

    for (int i = 0; i < 512; i++) {
        prev[i] = (uint8_t)(i & 0xFF);
        curr[i] = (uint8_t)((i + 37) & 0xFF);
    }

    netc_delta_encode(prev, curr, residual, 512);
    netc_delta_decode(prev, residual, recovered, 512);

    assert_bytes_equal(curr, recovered, 512, "delta roundtrip large");
}

void test_delta_xor_regions(void) {
    /* 1.2 Field-class XOR regions: offsets 0-15 and 64-255 */
    uint8_t prev[256], curr[256], residual[256];
    memset(prev, 0xAA, 256);
    memset(curr, 0x55, 256);

    netc_delta_encode(prev, curr, residual, 256);

    /* XOR region: offsets 0-15 → 0xAA ^ 0x55 = 0xFF */
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)0xFF, residual[i], "HEADER region should use XOR");
    }
    /* SUB region: offsets 16-63 → 0x55 - 0xAA = 0xAB (wrapping) */
    for (int i = 16; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)0xAB, residual[i], "SUBHEADER region should use SUB");
    }
    /* XOR region: offsets 64-255 → 0xFF */
    for (int i = 64; i < 256; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)0xFF, residual[i], "BODY region should use XOR");
    }
}

void test_delta_sub_regions(void) {
    /* 1.3 Field-class SUB regions: offsets 16-63, 256+ */
    uint8_t prev[320], curr[320], residual[320];
    memset(prev, 0x10, 320);
    memset(curr, 0x30, 320);

    netc_delta_encode(prev, curr, residual, 320);

    /* SUB region: 16-63 → 0x30 - 0x10 = 0x20 */
    for (int i = 16; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, residual[i], "SUBHEADER SUB");
    }
    /* TAIL region: 256+ → 0x30 - 0x10 = 0x20 */
    for (int i = 256; i < 320; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, residual[i], "TAIL SUB");
    }
}

void test_delta_zero_prev_identity(void) {
    /* 1.4 Zero prev → residual equals curr (XOR with 0 = curr, SUB with 0 = curr) */
    uint8_t prev[256], curr[256], residual[256];
    memset(prev, 0x00, 256);
    for (int i = 0; i < 256; i++) curr[i] = (uint8_t)(i);

    netc_delta_encode(prev, curr, residual, 256);

    assert_bytes_equal(curr, residual, 256, "zero prev: residual == curr");
}

void test_delta_same_prev_curr_zero_residual(void) {
    /* 1.5 Same prev and curr → residual is all zeros (XOR = 0, SUB = 0) */
    uint8_t data[256], residual[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)(i);

    netc_delta_encode(data, data, residual, 256);

    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, residual[i], "same prev/curr: residual[i] == 0");
    }
}

void test_delta_wraparound_subtraction(void) {
    /* 1.6 SUB wraps at 256: curr=0x00 prev=0x01 → residual=0xFF, recover → 0x00 */
    uint8_t prev[20], curr[20], residual[20], recovered[20];
    memset(prev, 0, 20);
    memset(curr, 0, 20);

    /* Offset 16 is in SUBHEADER (SUB region) */
    prev[16] = 0x01;
    curr[16] = 0x00;  /* curr - prev = 0x00 - 0x01 = 0xFF (wrapping) */

    netc_delta_encode(prev, curr, residual, 20);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)0xFF, residual[16], "SUB wrap: residual[16]");

    netc_delta_decode(prev, residual, recovered, 20);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, recovered[16], "SUB unwrap: recovered[16]");
}

void test_delta_decode_inplace(void) {
    /* 1.7 Decode in-place: curr == residual buffer is allowed */
    uint8_t prev[128], curr_orig[128];
    uint8_t residual[128]; /* will be decoded in-place */

    for (int i = 0; i < 128; i++) {
        prev[i]      = (uint8_t)(i & 0xFF);
        curr_orig[i] = (uint8_t)((i + 99) & 0xFF);
    }

    netc_delta_encode(prev, curr_orig, residual, 128);

    /* Decode in-place: output buffer == residual buffer */
    netc_delta_decode(prev, residual, residual, 128);

    assert_bytes_equal(curr_orig, residual, 128, "decode in-place");
}

void test_delta_tail_region_sub(void) {
    /* 1.8 Large packet: TAIL region (offset 256+) uses SUB */
    uint8_t prev[300], curr[300], residual[300], recovered[300];

    for (int i = 0; i < 300; i++) {
        prev[i] = (uint8_t)(i & 0xFF);
        curr[i] = (uint8_t)((i + 200) & 0xFF);
    }

    netc_delta_encode(prev, curr, residual, 300);
    netc_delta_decode(prev, residual, recovered, 300);

    /* Verify TAIL region specifically */
    for (int i = 256; i < 300; i++) {
        uint8_t expected_residual = (uint8_t)(curr[i] - prev[i]);
        TEST_ASSERT_EQUAL_HEX8(expected_residual, residual[i]);
        TEST_ASSERT_EQUAL_HEX8(curr[i], recovered[i]);
    }
}

/* =========================================================================
 * 2. Pipeline integration — delta + tANS round-trip (stateful)
 * ========================================================================= */

void test_delta_pipeline_two_packets_roundtrip(void) {
    /* 2.1 Two sequential packets with delta enabled: both round-trip correctly */
    TEST_ASSERT_NOT_NULL(s_ctx);
    TEST_ASSERT_NOT_NULL(s_dict);

    uint8_t cbuf1[PKT_SIZE + 64], cbuf2[PKT_SIZE + 64];
    uint8_t dbuf1[PKT_SIZE], dbuf2[PKT_SIZE];
    size_t csz1, csz2, dsz1, dsz2;

    /* Compress packet 1 */
    netc_result_t r = netc_compress(s_ctx, s_pkt_base, PKT_SIZE,
                                    cbuf1, sizeof(cbuf1), &csz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    /* Compress packet 2 */
    r = netc_compress(s_ctx, s_pkt_next, PKT_SIZE,
                      cbuf2, sizeof(cbuf2), &csz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    /* Reset context and decompress both */
    netc_ctx_reset(s_ctx);

    r = netc_decompress(s_ctx, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(PKT_SIZE, dsz1);
    assert_bytes_equal(s_pkt_base, dbuf1, PKT_SIZE, "pkt1 decompress");

    r = netc_decompress(s_ctx, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(PKT_SIZE, dsz2);
    assert_bytes_equal(s_pkt_next, dbuf2, PKT_SIZE, "pkt2 decompress");
}

void test_delta_first_packet_no_delta_flag(void) {
    /* 2.2 First packet (no prior history) is compressed without delta flag */
    TEST_ASSERT_NOT_NULL(s_ctx);
    TEST_ASSERT_NOT_NULL(s_dict);

    uint8_t cbuf[PKT_SIZE + 64];
    size_t csz;

    netc_result_t r = netc_compress(s_ctx, s_pkt_base, PKT_SIZE,
                                    cbuf, sizeof(cbuf), &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_GREATER_OR_EQUAL(8, csz);

    /* Read the flags byte from the header (offset 4) */
    uint8_t flags = cbuf[4];
    TEST_ASSERT_FALSE_MESSAGE(flags & NETC_PKT_FLAG_DELTA,
                              "first packet should NOT have DELTA flag");
}

void test_delta_sequential_multi_packet_roundtrip(void) {
    /* 2.5 Multiple sequential packets all round-trip correctly */
    TEST_ASSERT_NOT_NULL(s_ctx);
    TEST_ASSERT_NOT_NULL(s_dict);

#define MULTI_COUNT 10
    uint8_t pkts[MULTI_COUNT][PKT_SIZE];
    uint8_t cbufs[MULTI_COUNT][PKT_SIZE + 64];
    size_t  cszs[MULTI_COUNT];

    /* Build correlated sequence: each packet is base + small increment */
    for (int p = 0; p < MULTI_COUNT; p++) {
        for (int i = 0; i < PKT_SIZE; i++) {
            pkts[p][i] = (uint8_t)(0x41 + (i & 0x0F) + p);
        }
    }

    /* Compress all packets with delta ctx */
    for (int p = 0; p < MULTI_COUNT; p++) {
        netc_result_t r = netc_compress(s_ctx, pkts[p], PKT_SIZE,
                                        cbufs[p], PKT_SIZE + 64, &cszs[p]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, r, "compress failed");
    }

    /* Reset and decompress — verify each packet */
    netc_ctx_reset(s_ctx);
    for (int p = 0; p < MULTI_COUNT; p++) {
        uint8_t dbuf[PKT_SIZE];
        size_t  dsz;
        netc_result_t r = netc_decompress(s_ctx, cbufs[p], cszs[p],
                                          dbuf, sizeof(dbuf), &dsz);
        char msg[64];
        snprintf(msg, sizeof(msg), "decompress failed at packet %d", p);
        TEST_ASSERT_EQUAL_INT_MESSAGE(NETC_OK, r, msg);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(PKT_SIZE, dsz, msg);
        assert_bytes_equal(pkts[p], dbuf, PKT_SIZE, msg);
    }
#undef MULTI_COUNT
}

void test_delta_ctx_reset_clears_history(void) {
    /* 2.6 ctx_reset clears delta history: next packet after reset has no DELTA flag */
    TEST_ASSERT_NOT_NULL(s_ctx);
    TEST_ASSERT_NOT_NULL(s_dict);

    uint8_t cbuf[PKT_SIZE + 64];
    size_t csz;

    /* Compress packet 1 to establish history */
    netc_compress(s_ctx, s_pkt_base, PKT_SIZE, cbuf, sizeof(cbuf), &csz);

    /* Reset context — clears prev_pkt history */
    netc_ctx_reset(s_ctx);

    /* Compress packet 2 — should behave as first packet (no delta) */
    netc_result_t r = netc_compress(s_ctx, s_pkt_next, PKT_SIZE,
                                    cbuf, sizeof(cbuf), &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    uint8_t flags = cbuf[4];
    TEST_ASSERT_FALSE_MESSAGE(flags & NETC_PKT_FLAG_DELTA,
                              "packet after ctx_reset should NOT have DELTA flag");
}

/* =========================================================================
 * 3. Pipeline integration — passthrough path with delta
 * ========================================================================= */

void test_delta_size_mismatch_skips_delta(void) {
    /* 3.2 Size mismatch between consecutive packets: delta is skipped */
    TEST_ASSERT_NOT_NULL(s_ctx);

    uint8_t pkt_small[64], pkt_large[256];
    uint8_t cbuf[512];
    size_t csz;

    memset(pkt_small, 0x41, sizeof(pkt_small));
    memset(pkt_large, 0x42, sizeof(pkt_large));

    /* Compress small packet (establishes history with size 64) */
    netc_compress(s_ctx, pkt_small, sizeof(pkt_small), cbuf, sizeof(cbuf), &csz);

    /* Compress large packet — sizes differ, delta must not apply */
    netc_result_t r = netc_compress(s_ctx, pkt_large, sizeof(pkt_large),
                                    cbuf, sizeof(cbuf), &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    uint8_t flags = cbuf[4];
    TEST_ASSERT_FALSE_MESSAGE(flags & NETC_PKT_FLAG_DELTA,
                              "size mismatch: delta flag must NOT be set");

    /* Both must still round-trip correctly (decompress in fresh ctx) */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
    netc_ctx_t *dctx = netc_ctx_create(s_dict, &cfg);
    TEST_ASSERT_NOT_NULL(dctx);

    /* Re-compress into two buffers for clean decompression test */
    uint8_t cbuf1[256], cbuf2[512];
    size_t csz1, csz2;
    netc_ctx_reset(s_ctx);
    netc_compress(s_ctx, pkt_small, sizeof(pkt_small), cbuf1, sizeof(cbuf1), &csz1);
    netc_compress(s_ctx, pkt_large, sizeof(pkt_large), cbuf2, sizeof(cbuf2), &csz2);

    uint8_t dbuf1[64], dbuf2[256];
    size_t dsz1, dsz2;
    r = netc_decompress(dctx, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    assert_bytes_equal(pkt_small, dbuf1, sizeof(pkt_small), "small pkt decompress");

    r = netc_decompress(dctx, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    assert_bytes_equal(pkt_large, dbuf2, sizeof(pkt_large), "large pkt decompress");

    netc_ctx_destroy(dctx);
}

/* =========================================================================
 * 4. Delta disabled
 * ========================================================================= */

void test_delta_disabled_no_flag(void) {
    /* 4.1 NETC_PKT_FLAG_DELTA is never set when cfg flag is absent */
    TEST_ASSERT_NOT_NULL(s_ctx_nodelta);
    TEST_ASSERT_NOT_NULL(s_dict);

    uint8_t cbuf1[PKT_SIZE + 64], cbuf2[PKT_SIZE + 64];
    size_t csz1, csz2;

    netc_compress(s_ctx_nodelta, s_pkt_base, PKT_SIZE, cbuf1, sizeof(cbuf1), &csz1);
    netc_compress(s_ctx_nodelta, s_pkt_next, PKT_SIZE, cbuf2, sizeof(cbuf2), &csz2);

    /* Neither packet should have DELTA flag */
    TEST_ASSERT_FALSE_MESSAGE(cbuf1[4] & NETC_PKT_FLAG_DELTA,
                              "no-delta ctx: pkt1 must not have DELTA flag");
    TEST_ASSERT_FALSE_MESSAGE(cbuf2[4] & NETC_PKT_FLAG_DELTA,
                              "no-delta ctx: pkt2 must not have DELTA flag");
}

void test_delta_disabled_roundtrip(void) {
    /* 4.2 Compression without delta flag round-trips correctly */
    TEST_ASSERT_NOT_NULL(s_ctx_nodelta);
    TEST_ASSERT_NOT_NULL(s_dict);

    uint8_t cbuf[PKT_SIZE + 64], dbuf[PKT_SIZE];
    size_t csz, dsz;

    netc_result_t r = netc_compress(s_ctx_nodelta, s_pkt_base, PKT_SIZE,
                                    cbuf, sizeof(cbuf), &csz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    netc_ctx_reset(s_ctx_nodelta);
    r = netc_decompress(s_ctx_nodelta, cbuf, csz, dbuf, sizeof(dbuf), &dsz);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(PKT_SIZE, dsz);
    assert_bytes_equal(s_pkt_base, dbuf, PKT_SIZE, "no-delta roundtrip");
}

/* =========================================================================
 * 5. Spec scenarios
 * ========================================================================= */

void test_delta_spec_residual_formula_sub(void) {
    /* 5.1 SUB region: residual[i] = (C[i] - P[i]) mod 256
     * Test with offset 20 (SUBHEADER = SUB region) */
    uint8_t prev[30], curr[30], residual[30], recovered[30];
    memset(prev, 0, 30);
    memset(curr, 0, 30);

    prev[20] = (uint8_t)0xC0;
    curr[20] = (uint8_t)0x30;
    /* Expected: (0x30 - 0xC0) mod 256 = 0x70 */

    netc_delta_encode(prev, curr, residual, 30);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0x70, residual[20]);

    netc_delta_decode(prev, residual, recovered, 30);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)0x30, recovered[20]);
}

void test_delta_spec_small_packet_no_delta(void) {
    /* 5.2 Delta disabled for packets < NETC_DELTA_MIN_SIZE */
    TEST_ASSERT_NOT_NULL(s_ctx);
    TEST_ASSERT_NOT_NULL(s_dict);

    /* First packet of NETC_DELTA_MIN_SIZE - 1 = 7 bytes */
    uint8_t small_pkt[7];
    memset(small_pkt, 0x41, sizeof(small_pkt));

    uint8_t cbuf1[64], cbuf2[64];
    size_t csz1, csz2;

    /* Compress a full-size packet first to establish history */
    netc_compress(s_ctx, s_pkt_base, PKT_SIZE, cbuf1, sizeof(cbuf1), &csz1);

    /* Now compress a tiny packet — delta must not apply even though history exists */
    /* (size mismatch also prevents it, but the min-size check fires first) */
    uint8_t tiny1[7], tiny2[7];
    memset(tiny1, 0x41, 7);
    memset(tiny2, 0x41, 7);

    /* Fresh ctx with delta, compress two tiny packets */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
    netc_ctx_t *tctx = netc_ctx_create(NULL, &cfg);  /* no dict → passthrough */
    TEST_ASSERT_NOT_NULL(tctx);

    netc_compress(tctx, tiny1, sizeof(tiny1), cbuf1, sizeof(cbuf1), &csz1);
    netc_compress(tctx, tiny2, sizeof(tiny2), cbuf2, sizeof(cbuf2), &csz2);

    /* Neither packet should have delta flag (too small) */
    TEST_ASSERT_FALSE_MESSAGE(cbuf1[4] & NETC_PKT_FLAG_DELTA,
                              "tiny pkt1: no DELTA flag");
    TEST_ASSERT_FALSE_MESSAGE(cbuf2[4] & NETC_PKT_FLAG_DELTA,
                              "tiny pkt2: no DELTA flag (< NETC_DELTA_MIN_SIZE)");

    netc_ctx_destroy(tctx);
}

void test_delta_exact_min_size_boundary(void) {
    /* Packets exactly at NETC_DELTA_MIN_SIZE: delta may apply on 2nd+ packet */
    uint8_t pkt1[NETC_DELTA_MIN_SIZE], pkt2[NETC_DELTA_MIN_SIZE];
    memset(pkt1, 0x41, NETC_DELTA_MIN_SIZE);
    memset(pkt2, 0x42, NETC_DELTA_MIN_SIZE);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
    netc_ctx_t *tctx = netc_ctx_create(NULL, &cfg); /* no dict: passthrough */
    TEST_ASSERT_NOT_NULL(tctx);

    uint8_t cbuf1[NETC_DELTA_MIN_SIZE + 64];
    uint8_t cbuf2[NETC_DELTA_MIN_SIZE + 64];
    uint8_t dbuf1[NETC_DELTA_MIN_SIZE];
    uint8_t dbuf2[NETC_DELTA_MIN_SIZE];
    size_t csz1, csz2, dsz1, dsz2;

    netc_result_t r = netc_compress(tctx, pkt1, NETC_DELTA_MIN_SIZE,
                                    cbuf1, sizeof(cbuf1), &csz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    r = netc_compress(tctx, pkt2, NETC_DELTA_MIN_SIZE,
                      cbuf2, sizeof(cbuf2), &csz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);

    /* Round-trip must be correct regardless of whether delta fired */
    netc_ctx_reset(tctx);

    r = netc_decompress(tctx, cbuf1, csz1, dbuf1, sizeof(dbuf1), &dsz1);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(NETC_DELTA_MIN_SIZE, dsz1);
    assert_bytes_equal(pkt1, dbuf1, NETC_DELTA_MIN_SIZE, "min size pkt1");

    r = netc_decompress(tctx, cbuf2, csz2, dbuf2, sizeof(dbuf2), &dsz2);
    TEST_ASSERT_EQUAL_INT(NETC_OK, r);
    TEST_ASSERT_EQUAL_size_t(NETC_DELTA_MIN_SIZE, dsz2);
    assert_bytes_equal(pkt2, dbuf2, NETC_DELTA_MIN_SIZE, "min size pkt2");

    netc_ctx_destroy(tctx);
}

void test_delta_roundtrip_all_zeros(void) {
    /* Edge case: all-zero prev and curr → residual all zero, decode recovers zero */
    uint8_t prev[128], curr[128], residual[128], recovered[128];
    memset(prev, 0, 128);
    memset(curr, 0, 128);

    netc_delta_encode(prev, curr, residual, 128);
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, residual[i], "zero/zero: residual[i]");
    }

    netc_delta_decode(prev, residual, recovered, 128);
    for (int i = 0; i < 128; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, recovered[i], "zero/zero: recovered[i]");
    }
}

void test_delta_roundtrip_all_ones(void) {
    /* All-0xFF prev, all-0x00 curr */
    uint8_t prev[256], curr[256], residual[256], recovered[256];
    memset(prev, 0xFF, 256);
    memset(curr, 0x00, 256);

    netc_delta_encode(prev, curr, residual, 256);
    netc_delta_decode(prev, residual, recovered, 256);

    assert_bytes_equal(curr, recovered, 256, "all-ones prev, all-zero curr roundtrip");
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* 1. Primitive tests */
    RUN_TEST(test_delta_roundtrip_small);
    RUN_TEST(test_delta_roundtrip_large);
    RUN_TEST(test_delta_xor_regions);
    RUN_TEST(test_delta_sub_regions);
    RUN_TEST(test_delta_zero_prev_identity);
    RUN_TEST(test_delta_same_prev_curr_zero_residual);
    RUN_TEST(test_delta_wraparound_subtraction);
    RUN_TEST(test_delta_decode_inplace);
    RUN_TEST(test_delta_tail_region_sub);

    /* 2. Pipeline integration */
    RUN_TEST(test_delta_pipeline_two_packets_roundtrip);
    RUN_TEST(test_delta_first_packet_no_delta_flag);
    RUN_TEST(test_delta_sequential_multi_packet_roundtrip);
    RUN_TEST(test_delta_ctx_reset_clears_history);

    /* 3. Passthrough + delta interactions */
    RUN_TEST(test_delta_size_mismatch_skips_delta);

    /* 4. Delta disabled */
    RUN_TEST(test_delta_disabled_no_flag);
    RUN_TEST(test_delta_disabled_roundtrip);

    /* 5. Spec scenarios */
    RUN_TEST(test_delta_spec_residual_formula_sub);
    RUN_TEST(test_delta_spec_small_packet_no_delta);
    RUN_TEST(test_delta_exact_min_size_boundary);
    RUN_TEST(test_delta_roundtrip_all_zeros);
    RUN_TEST(test_delta_roundtrip_all_ones);

    return UNITY_END();
}
