/**
 * netc_tans.c — tANS (FSE) codec implementation.
 *
 * Algorithm: tabular Asymmetric Numeral Systems (FSE variant).
 *
 * Table construction (FSE spread function, as in Zstd):
 *   step = (TABLE_SIZE>>1) + (TABLE_SIZE>>3) + 3  [coprime with TABLE_SIZE]
 *   Symbol occurrences are spread using step to ensure the state machine
 *   forms a single globally-traversable chain (no isolated cycles).
 *
 * Decode table (per slot pos, occurrence-rank k of symbol s):
 *   symbol           = s
 *   X_prev           = f + k  (encoder state that normalizes to this slot)
 *   nb_bits          = TABLE_LOG - floor_log2(f + k)
 *   next_state_base  = (f + k) << nb_bits  ∈ [TABLE_SIZE, 2*TABLE_SIZE)
 *
 * Encode (state X ∈ [TABLE_SIZE, 2*TABLE_SIZE), symbol s, freq f):
 *   fl    = floor_log2(f)
 *   nb_hi = TABLE_LOG - fl
 *   lower = f << nb_hi
 *   nb    = (X >= lower) ? nb_hi : (nb_hi - 1)
 *   flush nb low bits of X
 *   j     = (X >> nb) - f    [∈ [0, f)]
 *   X_new = TABLE_SIZE + encode_state[cumul[s] + j]
 */

#include "netc_tans.h"
#include "../util/netc_bitstream.h"
#include <string.h>

/* =========================================================================
 * Utilities
 * ========================================================================= */

/** floor_log2: position of highest set bit. Returns 0 for v==0 or v==1. */
static NETC_INLINE int floor_log2_u32(uint32_t v) {
    int n = 0;
    while (v > 1U) { v >>= 1; n++; }
    return n;
}

/* =========================================================================
 * netc_tans_build
 *
 * Uses FSE spread function to assign symbol slots, then builds decode and
 * encode tables. This ensures the global state-chain is traversable.
 * ========================================================================= */

int netc_tans_build(netc_tans_table_t *tbl, const netc_freq_table_t *freq) {
    if (tbl == NULL || freq == NULL) return -1;

    /* Validate sum */
    uint32_t total = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        total += freq->freq[s];
    }
    if (total != NETC_TANS_TABLE_SIZE) return -1;

    memcpy(&tbl->freq, freq, sizeof(netc_freq_table_t));
    memset(tbl->decode,       0, sizeof(tbl->decode));
    memset(tbl->encode_state, 0, sizeof(tbl->encode_state));
    memset(tbl->encode,       0, sizeof(tbl->encode));

    /* --- Step 1: Compute cumulative frequencies --- */
    uint16_t cumul[NETC_TANS_SYMBOLS + 1];
    cumul[0] = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        cumul[s + 1] = (uint16_t)(cumul[s] + freq->freq[s]);
    }

    /* --- Step 2: Build encode entries (nb_hi, cumul) per symbol --- */
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        int fl = floor_log2_u32(freq->freq[s]);
        int nb_hi = (int)NETC_TANS_TABLE_LOG - fl;
        if (nb_hi < 0) nb_hi = 0;
        tbl->encode[s].nb_hi = (uint8_t)nb_hi;
        tbl->encode[s].cumul = cumul[s];
    }

    /* --- Step 3: Generate spread table using FSE step function ---
     *
     * step = (TABLE_SIZE>>1) + (TABLE_SIZE>>3) + 3 = 2563.
     * GCD(2563, 4096) = 1 (2563 is odd), so all TABLE_SIZE positions
     * are visited exactly once.
     *
     * We track the per-symbol occurrence rank in 'rank[]'.
     * spread_sym[pos] = symbol assigned to slot pos.
     * encode_state[cumul[s] + rank] = pos  (the k-th occurrence of s is at slot pos).
     */
    uint8_t  spread_sym[NETC_TANS_TABLE_SIZE];
    uint16_t rank[NETC_TANS_SYMBOLS];
    memset(rank, 0, sizeof(rank));
    memset(spread_sym, 0, sizeof(spread_sym));

    uint32_t pos = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        for (uint32_t k = 0; k < f; k++) {
            spread_sym[pos] = (uint8_t)s;
            /* Record that the k-th occurrence of symbol s is at slot pos */
            tbl->encode_state[cumul[s] + k] = (uint16_t)pos;
            pos = (pos + NETC_TANS_SPREAD_STEP) & (NETC_TANS_TABLE_SIZE - 1U);
        }
    }

    /* --- Step 4: Build decode table ---
     *
     * For each slot pos: symbol = spread_sym[pos].
     * Find the rank of this occurrence (we already stored it in encode_state,
     * but need to recover rank from the slot → use a per-symbol counter).
     *
     * Alternatively, build rank[] during step 3, then pass it here.
     * We reset rank[] and use a second pass.
     */
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) rank[s] = 0;

    /* Reset pos and redo the spread to build the decode table */
    pos = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        for (uint32_t k = 0; k < f; k++) {
            /* slot 'pos' is the k-th occurrence of symbol s */
            uint32_t X_prev = f + k;  /* encoder state in [f, 2*f) */
            int nb_j = (int)NETC_TANS_TABLE_LOG - floor_log2_u32(X_prev);
            if (nb_j < 0) nb_j = 0;

            tbl->decode[pos].symbol         = (uint8_t)s;
            tbl->decode[pos].nb_bits        = (uint8_t)nb_j;
            /* next_state_base = X_prev << nb_j ∈ [TABLE_SIZE, 2*TABLE_SIZE) */
            tbl->decode[pos].next_state_base = (uint16_t)(X_prev << (uint32_t)nb_j);

            pos = (pos + NETC_TANS_SPREAD_STEP) & (NETC_TANS_TABLE_SIZE - 1U);
        }
    }

    tbl->valid = 1;
    return 0;
}

/* =========================================================================
 * netc_tans_encode
 *
 * For each symbol s (processing src right-to-left):
 *   f        = freq[s]
 *   fl       = floor_log2(f)
 *   nb_hi    = TABLE_LOG - fl
 *   lower    = f << nb_hi   (threshold for nb_lo vs nb_hi)
 *   nb       = (X >= lower) ? nb_hi : nb_hi - 1
 *   flush nb low bits of X
 *   j        = (X >> nb) - f
 *   X_new    = TABLE_SIZE + encode_state[cumul[s] + j]
 *
 * Returns final state (initial state for decoder), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode(
    const netc_tans_table_t *tbl,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                 initial_state)
{
    if (!tbl || !tbl->valid || !src || !bsw || src_size == 0) return 0;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE) X = NETC_TANS_TABLE_SIZE;

    for (size_t i = src_size; i-- > 0; ) {
        uint8_t  sym = src[i];
        uint32_t f   = tbl->freq.freq[sym];
        if (f == 0) return 0;  /* symbol absent from table */

        int      nb_hi = (int)tbl->encode[sym].nb_hi;
        uint32_t lower = f << (uint32_t)nb_hi;
        int      nb;
        uint32_t j;

        /* Determine nb: use nb_hi when X in [lower, 2*TABLE_SIZE),
         * use nb_hi-1 (=nb_lo) when X in [TABLE_SIZE, lower).
         * Special case: nb_hi == 0 means f == TABLE_SIZE (single symbol). */
        if (nb_hi == 0 || X >= lower) {
            nb = nb_hi;
            j  = (X >> (uint32_t)nb) - f;
        } else {
            nb = nb_hi - 1;
            j  = (X >> (uint32_t)nb) - f;
        }

        /* Flush nb low bits of X */
        if (nb > 0) {
            if (netc_bsw_write(bsw, X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
                return 0;
        }

        /* Transition: look up the spread slot for the j-th occurrence of sym */
        X = (uint32_t)NETC_TANS_TABLE_SIZE
            + (uint32_t)tbl->encode_state[(uint32_t)tbl->encode[sym].cumul + j];
    }

    return X;
}

/* =========================================================================
 * netc_tans_decode
 *
 * Decode step per symbol (reading bitstream backward):
 *   slot = X - TABLE_SIZE
 *   s    = decode[slot].symbol
 *   nb   = decode[slot].nb_bits
 *   bits = read(nb) from bsr
 *   X    = decode[slot].next_state_base + bits
 *
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode(
    const netc_tans_table_t *tbl,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state)
{
    if (!tbl || !tbl->valid || !bsr || !dst || dst_size == 0) return -1;

    uint32_t X = initial_state;

    for (size_t i = 0; i < dst_size; i++) {
        if (X < NETC_TANS_TABLE_SIZE || X >= 2U * NETC_TANS_TABLE_SIZE) return -1;

        uint32_t slot = X - NETC_TANS_TABLE_SIZE;
        const netc_tans_decode_entry_t *d = &tbl->decode[slot];

        dst[i] = d->symbol;

        int      nb       = d->nb_bits;
        uint32_t bits_val = 0;
        if (nb > 0) {
            if (netc_bsr_read(bsr, nb, &bits_val) != 0) return -1;
        }

        X = (uint32_t)d->next_state_base + bits_val;
    }

    return 0;
}
