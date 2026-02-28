/**
 * netc_adaptive.c -- Adaptive tANS table rebuild.
 *
 * Blends accumulated per-bucket byte frequencies with the dictionary
 * baseline to produce new normalized frequency tables, then rebuilds
 * the tANS encode/decode structures.
 *
 * Blend formula per symbol s in bucket b:
 *   blended[s] = alpha * accum_freq[b][s] + (1-alpha) * dict_freq[b][s]
 *
 * where alpha = NETC_ADAPTIVE_ALPHA_NUM / NETC_ADAPTIVE_ALPHA_DEN (default 3/4).
 * This weights accumulated (observed) data more heavily than the dict baseline,
 * while keeping the baseline as a stability anchor for rare symbols.
 */

#include "netc_adaptive.h"
#include "netc_tans.h"
#include <string.h>

/* =========================================================================
 * freq_normalize_adaptive -- scale blended counts to sum to TABLE_SIZE.
 *
 * Same two-phase approach as netc_dict.c's freq_normalize:
 *   Phase 1: floor of 1 for all 256 symbols
 *   Phase 2: proportional distribution of remaining 3840 slots
 * ========================================================================= */

static void freq_normalize_adaptive(
    const uint64_t blended[NETC_TANS_SYMBOLS],
    uint64_t        total,
    uint16_t        out[NETC_TANS_SYMBOLS])
{
    int s;

    for (s = 0; s < (int)NETC_TANS_SYMBOLS; s++)
        out[s] = 1;

    if (total == 0) {
        /* No data accumulated -- uniform distribution */
        for (s = 0; s < (int)NETC_TANS_SYMBOLS; s++)
            out[s] = (uint16_t)(NETC_TANS_TABLE_SIZE / NETC_TANS_SYMBOLS);
        return;
    }

    {
        const uint32_t budget = NETC_TANS_TABLE_SIZE - NETC_TANS_SYMBOLS;
        uint32_t table_sum = NETC_TANS_SYMBOLS;
        int      max_sym   = 0;
        uint32_t max_val   = 1;

        for (s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
            uint64_t bonus;
            if (blended[s] == 0) continue;

            bonus = (blended[s] * (uint64_t)budget) / total;
            if (bonus > 0xFFFEU) bonus = 0xFFFEU;

            out[s]     = (uint16_t)(1U + (uint32_t)bonus);
            table_sum += (uint32_t)bonus;

            if (out[s] > max_val) {
                max_val = out[s];
                max_sym = s;
            }
        }

        if (table_sum < NETC_TANS_TABLE_SIZE) {
            out[max_sym] = (uint16_t)(out[max_sym] + (NETC_TANS_TABLE_SIZE - table_sum));
        } else if (table_sum > NETC_TANS_TABLE_SIZE) {
            uint32_t excess = table_sum - NETC_TANS_TABLE_SIZE;
            if (out[max_sym] > (uint16_t)excess + 1U) {
                out[max_sym] = (uint16_t)(out[max_sym] - (uint16_t)excess);
            } else {
                /* Spread excess across multiple symbols */
                for (s = 0; s < (int)NETC_TANS_SYMBOLS && excess > 0; s++) {
                    if (out[s] > 1) { out[s]--; excess--; }
                }
            }
        }
    }
}

/* =========================================================================
 * netc_adaptive_tables_rebuild
 * ========================================================================= */

void netc_adaptive_tables_rebuild(netc_ctx_t *ctx)
{
    uint32_t b;
    if (!ctx || !ctx->adapt_freq || !ctx->adapt_tables || !ctx->dict) return;

    {
        const uint32_t *accum = ctx->adapt_freq;        /* [NETC_CTX_COUNT][256] flat */
        const uint32_t *accum_total = ctx->adapt_total;  /* [NETC_CTX_COUNT] */
        const netc_tans_table_t *dict_tables = ctx->dict->tables;

        for (b = 0; b < NETC_CTX_COUNT; b++) {
            const uint32_t *bucket_freq = &accum[b * 256];
            uint32_t bucket_total = accum_total[b];

            /* Blend accumulated frequencies with dict baseline */
            uint64_t blended[NETC_TANS_SYMBOLS];
            uint64_t blended_total = 0;
            int s;

            for (s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
                /* accum contribution: weighted by ALPHA_NUM */
                uint64_t a = (uint64_t)bucket_freq[s] * NETC_ADAPTIVE_ALPHA_NUM;
                /* dict baseline contribution: weighted by (DEN - NUM).
                 * Scale dict freq (normalized to 4096) by bucket_total to make
                 * it comparable to raw counts. If bucket_total is 0, use dict only. */
                uint64_t d;
                if (bucket_total > 0) {
                    d = ((uint64_t)dict_tables[b].freq.freq[s] * bucket_total / NETC_TANS_TABLE_SIZE)
                        * (NETC_ADAPTIVE_ALPHA_DEN - NETC_ADAPTIVE_ALPHA_NUM);
                } else {
                    d = (uint64_t)dict_tables[b].freq.freq[s];
                    a = 0;
                }
                blended[s] = a + d;
                blended_total += blended[s];
            }

            /* Normalize and rebuild */
            {
                netc_freq_table_t ft;
                freq_normalize_adaptive(blended, blended_total, ft.freq);
                int rc = netc_tans_build(&ctx->adapt_tables[b], &ft);
                if (rc != 0) {
                    /* Build failed — table is in inconsistent state.
                     * Re-clone from dict to maintain decodability. */
                    ctx->adapt_tables[b] = ctx->dict->tables[b];
                }
            }
        }
    }
}
