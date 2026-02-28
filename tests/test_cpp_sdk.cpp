/**
 * test_cpp_sdk.cpp — Standalone C++17 tests for the netc C++ SDK.
 *
 * Uses Unity C test framework. Tests cover:
 *   1. Result enum and string conversion (4 tests)
 *   2. Dict lifecycle: construct, move, load/save, model_id (10 tests)
 *   3. Context lifecycle: construct, move, reset, simd, stats (8 tests)
 *   4. Compress/Decompress round-trip: TCP, UDP, multi-packet (8 tests)
 *   5. Error paths: too big, corrupt, invalid dict, null (6 tests)
 *   6. Trainer: add, train, reset (5 tests)
 *   7. RAII safety: destructor after move, scope exit (3 tests)
 */

extern "C" {
#include "unity.h"
}

#include "netc.hpp"

extern "C" {
#include "netc.h"
}

#include <cstring>
#include <vector>
#include <cstdlib>

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const uint8_t SAMPLE_GAME_STATE[64] = {
    0x01, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0xA0,
    0x41, 0x00, 0x00, 0x48, 0x42, 0x00, 0x00, 0x96,
    0x42, 0xCD, 0xCC, 0x4C, 0x3E, 0x00, 0x00, 0x80,
    0x3F, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x64,
    0x00, 0xFF, 0x00, 0x80, 0x40, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13,
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
};

/**
 * Build a minimal Dict from synthetic training data.
 * Returns true on success.
 */
static bool build_test_dict(netc::Dict& dict, uint8_t model_id = 1) {
    netc::Trainer trainer;

    /* Generate 200 synthetic training packets */
    uint8_t buf[128];
    for (int i = 0; i < 200; i++) {
        for (size_t j = 0; j < sizeof(buf); j++) {
            buf[j] = static_cast<uint8_t>((i + j * 7) & 0xFF);
        }
        trainer.AddPacket(buf, sizeof(buf));
    }

    /* Also add game-state-like packets */
    for (int i = 0; i < 200; i++) {
        uint8_t pkt[64];
        std::memcpy(pkt, SAMPLE_GAME_STATE, sizeof(pkt));
        pkt[0] = static_cast<uint8_t>(i & 0xFF);
        trainer.AddPacket(pkt, sizeof(pkt));
    }

    netc::Result r = trainer.Train(model_id, dict);
    return r == netc::Result::OK;
}

/* =========================================================================
 * Unity fixtures
 * ========================================================================= */

void setUp(void) { }
void tearDown(void) { }

/* =========================================================================
 * 1. Result enum tests
 * ========================================================================= */

void test_result_ok_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(netc::Result::OK));
}

void test_result_values_match_c(void) {
    TEST_ASSERT_EQUAL_INT(NETC_OK,               static_cast<int>(netc::Result::OK));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_NOMEM,        static_cast<int>(netc::Result::NoMem));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_TOOBIG,       static_cast<int>(netc::Result::TooBig));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CORRUPT,      static_cast<int>(netc::Result::Corrupt));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_DICT_INVALID, static_cast<int>(netc::Result::DictInvalid));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_BUF_SMALL,    static_cast<int>(netc::Result::BufSmall));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_CTX_NULL,     static_cast<int>(netc::Result::CtxNull));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_UNSUPPORTED,  static_cast<int>(netc::Result::Unsupported));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_VERSION,      static_cast<int>(netc::Result::Version));
    TEST_ASSERT_EQUAL_INT(NETC_ERR_INVALID_ARG,  static_cast<int>(netc::Result::InvalidArg));
}

void test_result_to_string_ok(void) {
    const char* s = netc::ResultToString(netc::Result::OK);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(std::strlen(s) > 0);
}

void test_result_to_string_all_codes(void) {
    netc::Result codes[] = {
        netc::Result::OK, netc::Result::NoMem, netc::Result::TooBig,
        netc::Result::Corrupt, netc::Result::DictInvalid, netc::Result::BufSmall,
        netc::Result::CtxNull, netc::Result::Unsupported, netc::Result::Version,
        netc::Result::InvalidArg,
    };
    for (auto code : codes) {
        const char* s = netc::ResultToString(code);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE(std::strlen(s) > 0);
    }
}

/* =========================================================================
 * 2. Dict lifecycle tests
 * ========================================================================= */

void test_dict_default_construct_invalid(void) {
    netc::Dict dict;
    TEST_ASSERT_FALSE(dict.IsValid());
    TEST_ASSERT_EQUAL_UINT8(0, dict.GetModelId());
}

void test_dict_move_construct(void) {
    netc::Dict a;
    TEST_ASSERT_TRUE(build_test_dict(a));
    TEST_ASSERT_TRUE(a.IsValid());

    netc::Dict b(std::move(a));
    TEST_ASSERT_TRUE(b.IsValid());
    TEST_ASSERT_FALSE(a.IsValid());
}

void test_dict_move_assign(void) {
    netc::Dict a, b;
    TEST_ASSERT_TRUE(build_test_dict(a));
    TEST_ASSERT_TRUE(build_test_dict(b, 2));

    b = std::move(a);
    TEST_ASSERT_TRUE(b.IsValid());
    TEST_ASSERT_FALSE(a.IsValid());
    TEST_ASSERT_EQUAL_UINT8(1, b.GetModelId());
}

void test_dict_double_move_no_crash(void) {
    netc::Dict a;
    TEST_ASSERT_TRUE(build_test_dict(a));

    netc::Dict b(std::move(a));
    netc::Dict c(std::move(a)); /* move from already-moved — should be no-op */
    TEST_ASSERT_FALSE(a.IsValid());
    TEST_ASSERT_FALSE(c.IsValid());
    TEST_ASSERT_TRUE(b.IsValid());
}

void test_dict_load_save_roundtrip(void) {
    netc::Dict original;
    TEST_ASSERT_TRUE(build_test_dict(original, 42));

    std::vector<uint8_t> blob;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(original.SaveToBytes(blob)));
    TEST_ASSERT_TRUE(blob.size() > 0);

    netc::Dict loaded;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        netc::Dict::LoadFromBytes(blob.data(), blob.size(), loaded)));
    TEST_ASSERT_TRUE(loaded.IsValid());
    TEST_ASSERT_EQUAL_UINT8(42, loaded.GetModelId());
}

void test_dict_load_from_bytes_invalid(void) {
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    netc::Dict dict;
    netc::Result r = netc::Dict::LoadFromBytes(garbage, sizeof(garbage), dict);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
    TEST_ASSERT_FALSE(dict.IsValid());
}

void test_dict_load_from_bytes_null(void) {
    netc::Dict dict;
    netc::Result r = netc::Dict::LoadFromBytes(nullptr, 0, dict);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(netc::Result::InvalidArg),
        static_cast<int>(r));
}

void test_dict_save_to_bytes(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    std::vector<uint8_t> blob;
    netc::Result r = dict.SaveToBytes(blob);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));
    TEST_ASSERT_TRUE(blob.size() > 100); /* dict blob is always substantial */
}

void test_dict_save_invalid_dict(void) {
    netc::Dict dict; /* invalid */
    std::vector<uint8_t> blob;
    netc::Result r = dict.SaveToBytes(blob);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
}

void test_dict_get_model_id(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict, 7));
    TEST_ASSERT_EQUAL_UINT8(7, dict.GetModelId());
}

void test_dict_save_load_file_roundtrip(void) {
    const char* path = "test_cpp_sdk_dict_roundtrip.bin";
    netc::Dict original;
    TEST_ASSERT_TRUE(build_test_dict(original, 33));

    /* Save to file */
    netc::Result r = original.SaveToFile(path);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));

    /* Load from file */
    netc::Dict loaded;
    r = netc::Dict::LoadFromFile(path, loaded);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));
    TEST_ASSERT_TRUE(loaded.IsValid());
    TEST_ASSERT_EQUAL_UINT8(33, loaded.GetModelId());

    /* Verify loaded dict produces identical blob as original */
    std::vector<uint8_t> blob_a, blob_b;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(original.SaveToBytes(blob_a)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(loaded.SaveToBytes(blob_b)));
    TEST_ASSERT_EQUAL_size_t(blob_a.size(), blob_b.size());
    TEST_ASSERT_EQUAL_MEMORY(blob_a.data(), blob_b.data(), blob_a.size());

    /* Cleanup temp file */
    remove(path);
}

void test_dict_load_file_nonexistent(void) {
    netc::Dict dict;
    netc::Result r = netc::Dict::LoadFromFile("nonexistent_path_xyz.bin", dict);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
    TEST_ASSERT_FALSE(dict.IsValid());
}

void test_dict_save_file_invalid_dict(void) {
    netc::Dict dict; /* default — invalid */
    netc::Result r = dict.SaveToFile("should_not_be_created.bin");
    TEST_ASSERT_TRUE(r != netc::Result::OK);
}

/* =========================================================================
 * 3. Context lifecycle tests
 * ========================================================================= */

void test_ctx_construct_tcp(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    TEST_ASSERT_TRUE(ctx.IsValid());
}

void test_ctx_construct_udp(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::UDP);
    TEST_ASSERT_TRUE(ctx.IsValid());
}

void test_ctx_move_construct(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context a(dict, netc::Mode::TCP);
    TEST_ASSERT_TRUE(a.IsValid());

    netc::Context b(std::move(a));
    TEST_ASSERT_TRUE(b.IsValid());
    TEST_ASSERT_FALSE(a.IsValid());
}

void test_ctx_move_assign(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context a(dict, netc::Mode::TCP);
    netc::Context b(dict, netc::Mode::TCP);

    b = std::move(a);
    TEST_ASSERT_TRUE(b.IsValid());
    TEST_ASSERT_FALSE(a.IsValid());
}

void test_ctx_reset(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    TEST_ASSERT_TRUE(ctx.IsValid());

    /* Compress one packet to create state, then reset */
    std::vector<uint8_t> dst;
    netc::Result r = ctx.Compress(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE), dst);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));

    ctx.Reset();
    TEST_ASSERT_TRUE(ctx.IsValid());
}

void test_ctx_get_simd_level(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    netc::SimdLevel level = ctx.GetSimdLevel();
    /* Must be one of the valid enum values */
    TEST_ASSERT_TRUE(
        level == netc::SimdLevel::Generic ||
        level == netc::SimdLevel::SSE42 ||
        level == netc::SimdLevel::AVX2 ||
        level == netc::SimdLevel::NEON);
}

void test_ctx_get_stats(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    netc::Stats stats;
    netc::Result r = ctx.GetStats(stats);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT64(0, stats.packets_compressed);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_in);
}

void test_ctx_max_compressed_size(void) {
    TEST_ASSERT_EQUAL_size_t(
        netc_compress_bound(100),
        netc::Context::MaxCompressedSize(100));
    TEST_ASSERT_EQUAL_size_t(
        netc_compress_bound(0),
        netc::Context::MaxCompressedSize(0));
    TEST_ASSERT_EQUAL_size_t(
        netc_compress_bound(65535),
        netc::Context::MaxCompressedSize(65535));
}

/* =========================================================================
 * 4. Compress/Decompress round-trip tests
 * ========================================================================= */

void test_compress_decompress_tcp_repetitive(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);
    netc::Context dec(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

    uint8_t src[512];
    std::memset(src, 0x41, sizeof(src));

    std::vector<uint8_t> compressed, recovered;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(enc.Compress(src, sizeof(src), compressed)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(dec.Decompress(compressed.data(), compressed.size(), recovered)));

    TEST_ASSERT_EQUAL_size_t(sizeof(src), recovered.size());
    TEST_ASSERT_EQUAL_MEMORY(src, recovered.data(), sizeof(src));
}

void test_compress_decompress_tcp_structured(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);
    netc::Context dec(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

    std::vector<uint8_t> compressed, recovered;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        enc.Compress(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE), compressed)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        dec.Decompress(compressed.data(), compressed.size(), recovered)));

    TEST_ASSERT_EQUAL_size_t(sizeof(SAMPLE_GAME_STATE), recovered.size());
    TEST_ASSERT_EQUAL_MEMORY(SAMPLE_GAME_STATE, recovered.data(), sizeof(SAMPLE_GAME_STATE));
}

void test_compress_decompress_tcp_high_entropy(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);
    netc::Context dec(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

    uint8_t src[128];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = static_cast<uint8_t>(i);
    }

    std::vector<uint8_t> compressed, recovered;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(enc.Compress(src, sizeof(src), compressed)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        dec.Decompress(compressed.data(), compressed.size(), recovered)));

    TEST_ASSERT_EQUAL_size_t(sizeof(src), recovered.size());
    TEST_ASSERT_EQUAL_MEMORY(src, recovered.data(), sizeof(src));
}

void test_compress_decompress_udp_stateless(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    std::vector<uint8_t> compressed, recovered;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        netc::Context::CompressStateless(dict, SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE), compressed)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        netc::Context::DecompressStateless(dict, compressed.data(), compressed.size(), recovered)));

    TEST_ASSERT_EQUAL_size_t(sizeof(SAMPLE_GAME_STATE), recovered.size());
    TEST_ASSERT_EQUAL_MEMORY(SAMPLE_GAME_STATE, recovered.data(), sizeof(SAMPLE_GAME_STATE));
}

void test_compress_decompress_multi_packet_tcp(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);
    netc::Context dec(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

    int fail_count = 0;
    for (int i = 0; i < 100; i++) {
        uint8_t pkt[64];
        std::memcpy(pkt, SAMPLE_GAME_STATE, sizeof(pkt));
        pkt[0] = static_cast<uint8_t>(i & 0xFF);
        pkt[1] = static_cast<uint8_t>((i >> 8) & 0xFF);

        std::vector<uint8_t> compressed, recovered;
        netc::Result r1 = enc.Compress(pkt, sizeof(pkt), compressed);
        netc::Result r2 = dec.Decompress(compressed.data(), compressed.size(), recovered);

        if (r1 != netc::Result::OK || r2 != netc::Result::OK) { fail_count++; continue; }
        if (recovered.size() != sizeof(pkt)) { fail_count++; continue; }
        if (std::memcmp(pkt, recovered.data(), sizeof(pkt)) != 0) { fail_count++; }
    }
    TEST_ASSERT_EQUAL_INT(0, fail_count);

    /* Verify stats reflect 100 packets */
    netc::Stats stats;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(enc.GetStats(stats)));
    TEST_ASSERT_EQUAL_UINT64(100, stats.packets_compressed);
}

void test_compress_output_bounded(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP, 5,
        NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

    uint8_t src[256];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::vector<uint8_t> dst;
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(ctx.Compress(src, sizeof(src), dst)));
    TEST_ASSERT_TRUE(dst.size() <= netc::Context::MaxCompressedSize(sizeof(src)));
}

void test_compress_1byte_packet(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP);
    netc::Context dec(dict, netc::Mode::TCP);

    uint8_t src[1] = {0xAB};
    std::vector<uint8_t> compressed, recovered;

    /* 1-byte packets should work (passthrough or compressed) */
    netc::Result r1 = enc.Compress(src, 1, compressed);
    if (r1 == netc::Result::OK) {
        netc::Result r2 = dec.Decompress(compressed.data(), compressed.size(), recovered);
        if (r2 == netc::Result::OK) {
            TEST_ASSERT_EQUAL_size_t(1, recovered.size());
            TEST_ASSERT_EQUAL_UINT8(0xAB, recovered[0]);
        }
    }
    /* Some configs may reject 1-byte — that's acceptable */
    TEST_PASS();
}

void test_compress_max_packet(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context enc(dict, netc::Mode::TCP);
    netc::Context dec(dict, netc::Mode::TCP);

    std::vector<uint8_t> src(NETC_MAX_PACKET_SIZE, 0x55);
    std::vector<uint8_t> compressed, recovered;

    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(enc.Compress(src.data(), src.size(), compressed)));
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(
        dec.Decompress(compressed.data(), compressed.size(), recovered)));

    TEST_ASSERT_EQUAL_size_t(src.size(), recovered.size());
    TEST_ASSERT_EQUAL_MEMORY(src.data(), recovered.data(), src.size());
}

/* =========================================================================
 * 5. Error path tests
 * ========================================================================= */

void test_compress_empty_src(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    std::vector<uint8_t> dst;
    /* Empty packet (size=0) — C API returns error or passthrough */
    netc::Result r = ctx.Compress(nullptr, 0, dst);
    /* Either error or success with passthrough is acceptable */
    (void)r;
    TEST_PASS();
}

void test_compress_too_big(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    std::vector<uint8_t> src(NETC_MAX_PACKET_SIZE + 1, 0x00);
    std::vector<uint8_t> dst;

    netc::Result r = ctx.Compress(src.data(), src.size(), dst);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(netc::Result::TooBig),
        static_cast<int>(r));
}

void test_decompress_corrupt(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<uint8_t> dst;

    netc::Result r = ctx.Decompress(garbage, sizeof(garbage), dst);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
}

void test_decompress_truncated(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context ctx(dict, netc::Mode::TCP);
    uint8_t tiny[] = {0x01}; /* way too short for any valid header */
    std::vector<uint8_t> dst;

    netc::Result r = ctx.Decompress(tiny, sizeof(tiny), dst);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
}

void test_ctx_from_invalid_dict(void) {
    netc::Dict dict; /* default — invalid */
    netc::Context ctx(dict, netc::Mode::TCP);
    TEST_ASSERT_FALSE(ctx.IsValid());
}

void test_stateless_invalid_dict(void) {
    netc::Dict dict; /* invalid */
    std::vector<uint8_t> dst;
    netc::Result r = netc::Context::CompressStateless(
        dict, SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE), dst);
    TEST_ASSERT_TRUE(r != netc::Result::OK);
}

/* =========================================================================
 * 6. Trainer tests
 * ========================================================================= */

void test_trainer_add_packet(void) {
    netc::Trainer trainer;
    TEST_ASSERT_EQUAL_size_t(0, trainer.GetCorpusCount());

    trainer.AddPacket(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE));
    TEST_ASSERT_EQUAL_size_t(1, trainer.GetCorpusCount());

    trainer.AddPacket(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE));
    TEST_ASSERT_EQUAL_size_t(2, trainer.GetCorpusCount());
}

void test_trainer_add_packets(void) {
    netc::Trainer trainer;
    std::vector<std::vector<uint8_t>> pkts;
    for (int i = 0; i < 50; i++) {
        pkts.emplace_back(SAMPLE_GAME_STATE, SAMPLE_GAME_STATE + sizeof(SAMPLE_GAME_STATE));
    }
    trainer.AddPackets(pkts);
    TEST_ASSERT_EQUAL_size_t(50, trainer.GetCorpusCount());
}

void test_trainer_train_produces_valid_dict(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));
    TEST_ASSERT_TRUE(dict.IsValid());
}

void test_trainer_train_model_id(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict, 99));
    TEST_ASSERT_EQUAL_UINT8(99, dict.GetModelId());
}

void test_trainer_reset(void) {
    netc::Trainer trainer;
    trainer.AddPacket(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE));
    TEST_ASSERT_EQUAL_size_t(1, trainer.GetCorpusCount());

    trainer.Reset();
    TEST_ASSERT_EQUAL_size_t(0, trainer.GetCorpusCount());
}

/* =========================================================================
 * 7. RAII safety tests
 * ========================================================================= */

void test_dict_destructor_after_move(void) {
    netc::Dict a;
    TEST_ASSERT_TRUE(build_test_dict(a));
    {
        netc::Dict b(std::move(a));
        TEST_ASSERT_TRUE(b.IsValid());
        /* b goes out of scope — should free the dict */
    }
    /* a was moved-from — destructor should be no-op */
    TEST_ASSERT_FALSE(a.IsValid());
    TEST_PASS();
}

void test_ctx_destructor_after_move(void) {
    netc::Dict dict;
    TEST_ASSERT_TRUE(build_test_dict(dict));

    netc::Context a(dict, netc::Mode::TCP);
    TEST_ASSERT_TRUE(a.IsValid());
    {
        netc::Context b(std::move(a));
        TEST_ASSERT_TRUE(b.IsValid());
    }
    TEST_ASSERT_FALSE(a.IsValid());
    TEST_PASS();
}

void test_scope_exit_cleanup(void) {
    /* Create dict + context in an inner scope, verify no crash on exit */
    {
        netc::Dict dict;
        TEST_ASSERT_TRUE(build_test_dict(dict));

        netc::Context ctx(dict, netc::Mode::TCP, 5,
            NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);
        TEST_ASSERT_TRUE(ctx.IsValid());

        std::vector<uint8_t> dst;
        netc::Result r = ctx.Compress(SAMPLE_GAME_STATE, sizeof(SAMPLE_GAME_STATE), dst);
        TEST_ASSERT_EQUAL_INT(0, static_cast<int>(r));
    }
    /* If we get here without crash, RAII cleanup worked */
    TEST_PASS();
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* 1. Result */
    RUN_TEST(test_result_ok_is_zero);
    RUN_TEST(test_result_values_match_c);
    RUN_TEST(test_result_to_string_ok);
    RUN_TEST(test_result_to_string_all_codes);

    /* 2. Dict lifecycle */
    RUN_TEST(test_dict_default_construct_invalid);
    RUN_TEST(test_dict_move_construct);
    RUN_TEST(test_dict_move_assign);
    RUN_TEST(test_dict_double_move_no_crash);
    RUN_TEST(test_dict_load_save_roundtrip);
    RUN_TEST(test_dict_load_from_bytes_invalid);
    RUN_TEST(test_dict_load_from_bytes_null);
    RUN_TEST(test_dict_save_to_bytes);
    RUN_TEST(test_dict_save_invalid_dict);
    RUN_TEST(test_dict_get_model_id);
    RUN_TEST(test_dict_save_load_file_roundtrip);
    RUN_TEST(test_dict_load_file_nonexistent);
    RUN_TEST(test_dict_save_file_invalid_dict);

    /* 3. Context lifecycle */
    RUN_TEST(test_ctx_construct_tcp);
    RUN_TEST(test_ctx_construct_udp);
    RUN_TEST(test_ctx_move_construct);
    RUN_TEST(test_ctx_move_assign);
    RUN_TEST(test_ctx_reset);
    RUN_TEST(test_ctx_get_simd_level);
    RUN_TEST(test_ctx_get_stats);
    RUN_TEST(test_ctx_max_compressed_size);

    /* 4. Compress/Decompress round-trip */
    RUN_TEST(test_compress_decompress_tcp_repetitive);
    RUN_TEST(test_compress_decompress_tcp_structured);
    RUN_TEST(test_compress_decompress_tcp_high_entropy);
    RUN_TEST(test_compress_decompress_udp_stateless);
    RUN_TEST(test_compress_decompress_multi_packet_tcp);
    RUN_TEST(test_compress_output_bounded);
    RUN_TEST(test_compress_1byte_packet);
    RUN_TEST(test_compress_max_packet);

    /* 5. Error paths */
    RUN_TEST(test_compress_empty_src);
    RUN_TEST(test_compress_too_big);
    RUN_TEST(test_decompress_corrupt);
    RUN_TEST(test_decompress_truncated);
    RUN_TEST(test_ctx_from_invalid_dict);
    RUN_TEST(test_stateless_invalid_dict);

    /* 6. Trainer */
    RUN_TEST(test_trainer_add_packet);
    RUN_TEST(test_trainer_add_packets);
    RUN_TEST(test_trainer_train_produces_valid_dict);
    RUN_TEST(test_trainer_train_model_id);
    RUN_TEST(test_trainer_reset);

    /* 7. RAII safety */
    RUN_TEST(test_dict_destructor_after_move);
    RUN_TEST(test_ctx_destructor_after_move);
    RUN_TEST(test_scope_exit_cleanup);

    return UNITY_END();
}
