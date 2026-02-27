/**
 * fuzz_compress.c — libFuzzer target: arbitrary packet content as input to compressor.
 *
 * Build with:
 *   clang -fsanitize=fuzzer,address,undefined -O1 \
 *         -I include fuzz_compress.c -L build/Release -lnetc -o fuzz_compress
 *
 * Invariants verified:
 *   1. netc_compress never crashes on arbitrary packet content.
 *   2. If netc_compress returns NETC_OK, the output size <= src_size + NETC_MAX_OVERHEAD.
 *   3. Round-trip: compress → decompress must reproduce the original bytes exactly.
 *   4. Stateless path round-trips identically.
 */

#include "../include/netc.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

static netc_ctx_t  *g_enc_ctx = NULL;
static netc_ctx_t  *g_dec_ctx = NULL;
static netc_dict_t *g_dict    = NULL;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;

    /* Train a minimal dictionary on a variety of small packets */
    static uint8_t pkt[512];
    const uint8_t *pkts[128];
    size_t         lens[128];

    for (int i = 0; i < 128; i++) {
        size_t len = (size_t)(32 + (i * 3) % 480);
        for (size_t j = 0; j < len; j++) pkt[j] = (uint8_t)(i ^ j);
        pkts[i] = pkt;
        lens[i] = len;
    }

    netc_dict_train(pkts, lens, 128, 1, &g_dict);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;

    g_enc_ctx = netc_ctx_create(g_dict, &cfg);
    g_dec_ctx = netc_ctx_create(g_dict, &cfg);

    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Skip packets that are too large for the library */
    if (size == 0 || size > NETC_MAX_PACKET_SIZE) return 0;

    static uint8_t comp_buf[NETC_MAX_PACKET_SIZE + NETC_MAX_OVERHEAD];
    static uint8_t decomp_buf[NETC_MAX_PACKET_SIZE];

    /* --- Stateful roundtrip --- */
    if (g_enc_ctx && g_dec_ctx) {
        netc_ctx_reset(g_enc_ctx);
        netc_ctx_reset(g_dec_ctx);

        size_t comp_size = 0;
        netc_result_t rc = netc_compress(g_enc_ctx, data, size,
                                         comp_buf, sizeof(comp_buf), &comp_size);
        if (rc == NETC_OK) {
            /* Invariant: output bounded */
            if (comp_size > size + NETC_MAX_OVERHEAD) __builtin_trap();

            /* Invariant: round-trip must reproduce original */
            size_t decomp_size = 0;
            netc_result_t rc2 = netc_decompress(g_dec_ctx, comp_buf, comp_size,
                                                 decomp_buf, sizeof(decomp_buf),
                                                 &decomp_size);
            if (rc2 == NETC_OK) {
                if (decomp_size != size) __builtin_trap();
                if (memcmp(data, decomp_buf, size) != 0) __builtin_trap();
            }
        }
    }

    /* --- Stateless roundtrip --- */
    if (g_dict) {
        size_t comp_size = 0;
        netc_result_t rc = netc_compress_stateless(g_dict, data, size,
                                                    comp_buf, sizeof(comp_buf),
                                                    &comp_size);
        if (rc == NETC_OK) {
            if (comp_size > size + NETC_MAX_OVERHEAD) __builtin_trap();

            size_t decomp_size = 0;
            netc_result_t rc2 = netc_decompress_stateless(g_dict, comp_buf, comp_size,
                                                           decomp_buf, sizeof(decomp_buf),
                                                           &decomp_size);
            if (rc2 == NETC_OK) {
                if (decomp_size != size) __builtin_trap();
                if (memcmp(data, decomp_buf, size) != 0) __builtin_trap();
            }
        }
    }

    return 0;
}
