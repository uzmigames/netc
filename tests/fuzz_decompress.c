/**
 * fuzz_decompress.c — libFuzzer target: arbitrary bytes as compressed input.
 *
 * Build with:
 *   clang -fsanitize=fuzzer,address,undefined -O1 \
 *         -I include fuzz_decompress.c -L build/Release -lnetc -o fuzz_decompress
 *
 * Run:
 *   ./fuzz_decompress -max_total_time=60 -max_len=1024
 *
 * AFL++ compatible:
 *   afl-clang-fast -fsanitize=address,undefined -O1 \
 *         -I include tests/fuzz_decompress.c build/Release/netc.lib -o fuzz_decompress
 *
 * Invariants verified on every iteration:
 *   1. netc_decompress never crashes (no SIGSEGV, SIGABRT, heap overflow).
 *   2. netc_decompress never hangs (libFuzzer timeout enforces this).
 *   3. If netc_decompress returns NETC_OK, *dst_size <= dst_cap (cap respected).
 *   4. netc_decompress_stateless invariants hold identically.
 */

#include "../include/netc.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Reuse a process-lifetime context and dictionary to avoid malloc pressure */
static netc_ctx_t  *g_ctx  = NULL;
static netc_dict_t *g_dict = NULL;

/* libFuzzer initializer (called once before LLVMFuzzerTestOneInput) */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;

    /* Train a minimal dictionary on 64 uniform packets */
    static uint8_t pkt[64];
    memset(pkt, 0x41, sizeof(pkt));
    const uint8_t *pkts[64];
    size_t         lens[64];
    for (int i = 0; i < 64; i++) { pkts[i] = pkt; lens[i] = 64; }

    netc_dict_train(pkts, lens, 64, 1, &g_dict);

    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
    g_ctx = netc_ctx_create(g_dict, &cfg);

    return 0;
}

/* libFuzzer entry point */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* dst must be large enough for any claimed original_size */
    static uint8_t dst[NETC_MAX_PACKET_SIZE + NETC_MAX_OVERHEAD];
    size_t dst_size = 0;

    /* --- Stateful decompress --- */
    if (g_ctx) {
        netc_ctx_reset(g_ctx);
        netc_result_t rc = netc_decompress(g_ctx, data, size,
                                           dst, sizeof(dst), &dst_size);
        if (rc == NETC_OK) {
            /* Invariant: dst_size must not exceed dst_cap */
            if (dst_size > sizeof(dst)) __builtin_trap();
        }
    }

    /* --- Stateless decompress --- */
    if (g_dict) {
        dst_size = 0;
        netc_result_t rc2 = netc_decompress_stateless(g_dict, data, size,
                                                       dst, sizeof(dst), &dst_size);
        if (rc2 == NETC_OK) {
            if (dst_size > sizeof(dst)) __builtin_trap();
        }
    }

    return 0;
}
