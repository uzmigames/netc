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
 * Encode (state X ∈ [TABLE_SIZE, 2*TABLE_SIZE), symbol s):
 *   freq  = encode[s].freq   (from merged entry — no separate freq[] lookup)
 *   nb_hi = encode[s].nb_hi
 *   lower = encode[s].lower  (pre-computed freq << nb_hi)
 *   nb    = (X >= lower) ? nb_hi : (nb_hi - 1)
 *   flush nb low bits of X
 *   j     = (X >> nb) - freq  [∈ [0, freq)]
 *   X_new = encode_state[encode[s].cumul + j]  (stores TABLE_SIZE+slot directly)
 */

#include "netc_tans.h"
#include "../util/netc_bitstream.h"
#include "../util/netc_platform.h"
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

    /* --- Step 2: Build encode entries (freq, lower, nb_hi, cumul) per symbol ---
     *
     * All fields are in one 8-byte struct so the encode hot loop fetches
     * freq, nb_hi, lower, and cumul in a single cache-line hit, eliminating
     * the separate netc_freq_table_t lookup.
     */
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        int fl     = floor_log2_u32(f);
        int nb_hi  = (int)NETC_TANS_TABLE_LOG - fl;
        if (nb_hi < 0) nb_hi = 0;
        tbl->encode[s].freq  = (uint16_t)f;
        tbl->encode[s].nb_hi = (uint8_t)nb_hi;
        tbl->encode[s].lower = (uint16_t)(f << (uint32_t)nb_hi);
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
            /* Store TABLE_SIZE + slot so the hot path assigns X directly
             * without an extra addition: X = encode_state[cumul+j] */
            tbl->encode_state[cumul[s] + k] = (uint16_t)(NETC_TANS_TABLE_SIZE + pos);
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
 * Hot loop (per symbol, right-to-left):
 *   e     = encode[sym]      — single 8-byte load: freq, lower, cumul, nb_hi
 *   nb    = (X >= e.lower) ? e.nb_hi : e.nb_hi - 1
 *   flush nb low bits of X into bitstream (word-at-a-time writer)
 *   j     = (X >> nb) - e.freq
 *   X     = encode_state[e.cumul + j]   — stores TABLE_SIZE+slot directly
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
        uint8_t sym = src[i];

        /* Single 8-byte load covers freq, lower, cumul, nb_hi */
        const netc_tans_encode_entry_t *e = &tbl->encode[sym];
        uint32_t f     = e->freq;
        uint32_t lower = e->lower;
        int      nb_hi = (int)e->nb_hi;

        /* Determine nb: nb_hi when X ∈ [lower, 2*TABLE_SIZE),
         * nb_hi-1 when X ∈ [TABLE_SIZE, lower).
         * When nb_hi == 0, f == TABLE_SIZE (single symbol) → nb = 0. */
        int      nb = (nb_hi == 0 || X >= lower) ? nb_hi : nb_hi - 1;
        uint32_t j  = (X >> (uint32_t)nb) - f;

        /* Flush nb low bits of X — word-at-a-time writer */
        if (nb > 0) {
            if (netc_bsw_write(bsw, X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
                return 0;
        }

        /* Transition: encode_state stores TABLE_SIZE+slot directly */
        X = (uint32_t)tbl->encode_state[(uint32_t)e->cumul + j];
    }

    return X;
}

/* =========================================================================
 * netc_tans_encode_step — one ANS encode step (inlined for x2 loop)
 * ========================================================================= */

static NETC_INLINE int tans_encode_step(
    const netc_tans_table_t *tbl, uint32_t *X, uint8_t sym, netc_bsw_t *bsw)
{
    const netc_tans_encode_entry_t *e = &tbl->encode[sym];
    uint32_t f     = e->freq;
    uint32_t lower = e->lower;
    int      nb_hi = (int)e->nb_hi;
    int      nb    = (nb_hi == 0 || *X >= lower) ? nb_hi : nb_hi - 1;
    uint32_t j     = (*X >> (uint32_t)nb) - f;
    if (nb > 0) {
        if (netc_bsw_write(bsw, *X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
            return -1;
    }
    *X = (uint32_t)tbl->encode_state[(uint32_t)e->cumul + j];
    return 0;
}

/* =========================================================================
 * netc_tans_encode_x2
 *
 * Dual-interleaved ANS encoder: processes pairs of symbols with two
 * independent states (X0, X1), breaking the serial dependency chain and
 * exposing instruction-level parallelism.
 *
 * Encoding order (right-to-left, same as single-state):
 *   Pass 1 (odd tail if src_size is odd): encode src[0] with X0
 *   Pass 2 (pairs, right-to-left): encode (src[i], src[i-1]) with (X0, X1)
 *
 * Both states emit bits into the same bitstream. The decoder reconstructs
 * with two interleaved states starting from (state0, state1).
 *
 * Returns 0 on success and writes final states to *out_state0, *out_state1.
 * Returns -1 on error (buffer overflow or invalid table).
 *
 * Wire format of the bitstream (read by netc_tans_decode_x2):
 *   bits are interleaved: X0-bits then X1-bits alternating per pair.
 *   The decoder must start with state1 then state0 (opposite order).
 * ========================================================================= */

int netc_tans_encode_x2(
    const netc_tans_table_t *tbl,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                *out_state0,
    uint32_t                *out_state1)
{
    if (!tbl || !tbl->valid || !src || !bsw || src_size < 2) return -1;
    if (!out_state0 || !out_state1) return -1;

    uint32_t X0 = NETC_TANS_TABLE_SIZE;
    uint32_t X1 = NETC_TANS_TABLE_SIZE;

    size_t i = src_size;

    /* If odd number of symbols, encode the last one with X0 first */
    if (i & 1u) {
        i--;
        if (tans_encode_step(tbl, &X0, src[i], bsw) != 0) return -1;
    }

    /* Process pairs right-to-left: encode src[i-1] with X1, src[i-2] with X0 */
    while (i >= 2) {
        i -= 2;
        if (tans_encode_step(tbl, &X1, src[i + 1], bsw) != 0) return -1;
        if (tans_encode_step(tbl, &X0, src[i],     bsw) != 0) return -1;
    }

    *out_state0 = X0;
    *out_state1 = X1;
    return 0;
}

/* =========================================================================
 * netc_tans_decode_x2
 *
 * Dual-interleaved ANS decoder: reconstructs symbols encoded by
 * netc_tans_encode_x2, using two independent states.
 *
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_x2(
    const netc_tans_table_t *tbl,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state0,
    uint32_t                 initial_state1)
{
    if (!tbl || !tbl->valid || !bsr || !dst || dst_size < 2) return -1;
    if (initial_state0 < NETC_TANS_TABLE_SIZE ||
        initial_state0 >= 2U * NETC_TANS_TABLE_SIZE) return -1;
    if (initial_state1 < NETC_TANS_TABLE_SIZE ||
        initial_state1 >= 2U * NETC_TANS_TABLE_SIZE) return -1;

    uint32_t X0 = initial_state0;
    uint32_t X1 = initial_state1;

    /* Prefetch both decode entries */
    NETC_PREFETCH(&tbl->decode[X0 - NETC_TANS_TABLE_SIZE]);
    NETC_PREFETCH(&tbl->decode[X1 - NETC_TANS_TABLE_SIZE]);

    size_t i = 0;

    /* If odd number of symbols, decode the first one from X0 */
    if (dst_size & 1u) {
        const netc_tans_decode_entry_t *d = &tbl->decode[X0 - NETC_TANS_TABLE_SIZE];
        dst[i++] = d->symbol;
        int      nb  = d->nb_bits;
        uint32_t bv  = 0;
        if (nb > 0 && netc_bsr_read(bsr, nb, &bv) != 0) return -1;
        X0 = (uint32_t)d->next_state_base + bv;
        NETC_PREFETCH(&tbl->decode[X0 - NETC_TANS_TABLE_SIZE]);
    }

    /* Process pairs: decode (X0, X1) into (dst[i], dst[i+1]) */
    while (i + 1 < dst_size) {
        const netc_tans_decode_entry_t *d0 = &tbl->decode[X0 - NETC_TANS_TABLE_SIZE];
        const netc_tans_decode_entry_t *d1 = &tbl->decode[X1 - NETC_TANS_TABLE_SIZE];

        dst[i]     = d0->symbol;
        dst[i + 1] = d1->symbol;

        int      nb0 = d0->nb_bits, nb1 = d1->nb_bits;
        uint32_t bv0 = 0, bv1 = 0;
        if (nb0 > 0 && netc_bsr_read(bsr, nb0, &bv0) != 0) return -1;
        if (nb1 > 0 && netc_bsr_read(bsr, nb1, &bv1) != 0) return -1;

        X0 = (uint32_t)d0->next_state_base + bv0;
        X1 = (uint32_t)d1->next_state_base + bv1;

        /* Prefetch next entries */
        NETC_PREFETCH(&tbl->decode[X0 - NETC_TANS_TABLE_SIZE]);
        NETC_PREFETCH(&tbl->decode[X1 - NETC_TANS_TABLE_SIZE]);

        i += 2;
    }

    return 0;
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

    /* Validate initial state once — table invariant guarantees all subsequent
     * transitions stay within [TABLE_SIZE, 2*TABLE_SIZE). */
    if (X < NETC_TANS_TABLE_SIZE || X >= 2U * NETC_TANS_TABLE_SIZE) return -1;

    /* Prefetch the first decode entry before the loop */
    NETC_PREFETCH(&tbl->decode[X - NETC_TANS_TABLE_SIZE]);

    for (size_t i = 0; i < dst_size; i++) {
        uint32_t slot = X - NETC_TANS_TABLE_SIZE;
        const netc_tans_decode_entry_t *d = &tbl->decode[slot];

        dst[i] = d->symbol;

        int      nb       = d->nb_bits;
        uint32_t bits_val = 0;
        if (nb > 0) {
            if (netc_bsr_read(bsr, nb, &bits_val) != 0) return -1;
        }

        X = (uint32_t)d->next_state_base + bits_val;

        /* Prefetch the next decode entry — hides ~4-cycle L1 load latency */
        if (i + 1 < dst_size) {
            NETC_PREFETCH(&tbl->decode[X - NETC_TANS_TABLE_SIZE]);
        }
    }

    return 0;
}

/* =========================================================================
 * netc_tans_encode_pctx
 *
 * Per-position context-adaptive ANS encoder.  Processes bytes in reverse
 * order (standard ANS), switching the probability table per byte offset:
 *   tbl = tables[netc_ctx_bucket(i)]
 *
 * This gives per-position entropy specialization (like MREG multi-region)
 * with ZERO descriptor overhead — wire format is [4B state][bitstream].
 *
 * Returns final state (initial state for decoder), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode_pctx(
    const netc_tans_table_t *tables,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                 initial_state)
{
    if (!tables || !src || !bsw || src_size == 0) return 0;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE) X = NETC_TANS_TABLE_SIZE;

    for (size_t i = src_size; i-- > 0; ) {
        uint32_t bucket = netc_ctx_bucket((uint32_t)i);
        const netc_tans_table_t *tbl = &tables[bucket];
        if (!tbl->valid) return 0;

        uint8_t sym = src[i];
        const netc_tans_encode_entry_t *e = &tbl->encode[sym];
        uint32_t f     = e->freq;
        uint32_t lower = e->lower;
        int      nb_hi = (int)e->nb_hi;

        if (f == 0) return 0; /* symbol not in this table */

        int      nb = (nb_hi == 0 || X >= lower) ? nb_hi : nb_hi - 1;
        uint32_t j  = (X >> (uint32_t)nb) - f;

        if (nb > 0) {
            if (netc_bsw_write(bsw, X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
                return 0;
        }

        X = (uint32_t)tbl->encode_state[(uint32_t)e->cumul + j];
    }

    return X;
}

/* =========================================================================
 * netc_tans_decode_pctx
 *
 * Per-position context-adaptive ANS decoder.  Decodes bytes in forward
 * order, switching the decode table per byte offset:
 *   tbl = tables[netc_ctx_bucket(i)]
 *
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_pctx(
    const netc_tans_table_t *tables,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state)
{
    if (!tables || !bsr || !dst || dst_size == 0) return -1;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE || X >= 2U * NETC_TANS_TABLE_SIZE) return -1;

    for (size_t i = 0; i < dst_size; i++) {
        uint32_t bucket = netc_ctx_bucket((uint32_t)i);
        const netc_tans_table_t *tbl = &tables[bucket];
        if (!tbl->valid) return -1;

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

/* =========================================================================
 * netc_tans_encode_pctx_bigram
 *
 * Per-position context-adaptive BIGRAM encoder.  Processes bytes in reverse
 * order (standard ANS), switching the probability table per byte using BOTH
 * position bucket AND bigram class:
 *   bucket = netc_ctx_bucket(i)
 *   bclass = netc_bigram_class(src[i-1], class_map)  (prev_byte at pos 0 = 0x00)
 *   tbl    = bigram_tables[bucket][bclass]  (fallback to unigram if invalid)
 *
 * Returns final state (initial state for decoder), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode_pctx_bigram(
    const netc_tans_table_t bigram_tables[][NETC_BIGRAM_CTX_COUNT],
    const netc_tans_table_t *unigram_tables,
    const uint8_t           *class_map,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                 initial_state)
{
    if (!bigram_tables || !unigram_tables || !src || !bsw || src_size == 0)
        return 0;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE) X = NETC_TANS_TABLE_SIZE;

    for (size_t i = src_size; i-- > 0; ) {
        uint32_t bucket = netc_ctx_bucket((uint32_t)i);

        /* Bigram context: previous byte (position i-1), or 0x00 at start */
        uint8_t prev_byte = (i > 0) ? src[i - 1] : 0x00u;
        uint32_t bclass = netc_bigram_class(prev_byte, class_map);

        const netc_tans_table_t *tbl = &bigram_tables[bucket][bclass];
        if (!tbl->valid) tbl = &unigram_tables[bucket];
        if (!tbl->valid) return 0;

        uint8_t sym = src[i];
        const netc_tans_encode_entry_t *e = &tbl->encode[sym];
        uint32_t f     = e->freq;
        uint32_t lower = e->lower;
        int      nb_hi = (int)e->nb_hi;

        if (f == 0) return 0;

        int      nb = (nb_hi == 0 || X >= lower) ? nb_hi : nb_hi - 1;
        uint32_t j  = (X >> (uint32_t)nb) - f;

        if (nb > 0) {
            if (netc_bsw_write(bsw, X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
                return 0;
        }

        X = (uint32_t)tbl->encode_state[(uint32_t)e->cumul + j];
    }

    return X;
}

/* =========================================================================
 * netc_tans_decode_pctx_bigram
 *
 * Per-position context-adaptive BIGRAM decoder.  Decodes bytes in forward
 * order, switching the decode table per byte using BOTH position bucket AND
 * bigram class derived from the previously decoded byte:
 *   bucket = netc_ctx_bucket(i)
 *   bclass = netc_bigram_class(dst[i-1], class_map)  (prev at pos 0 = 0x00)
 *   tbl    = bigram_tables[bucket][bclass]  (fallback to unigram if invalid)
 *
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_pctx_bigram(
    const netc_tans_table_t bigram_tables[][NETC_BIGRAM_CTX_COUNT],
    const netc_tans_table_t *unigram_tables,
    const uint8_t           *class_map,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state)
{
    if (!bigram_tables || !unigram_tables || !bsr || !dst || dst_size == 0)
        return -1;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE || X >= 2U * NETC_TANS_TABLE_SIZE) return -1;

    for (size_t i = 0; i < dst_size; i++) {
        uint32_t bucket = netc_ctx_bucket((uint32_t)i);

        /* Bigram context: previous decoded byte, or 0x00 at start */
        uint8_t prev_byte = (i > 0) ? dst[i - 1] : 0x00u;
        uint32_t bclass = netc_bigram_class(prev_byte, class_map);

        const netc_tans_table_t *tbl = &bigram_tables[bucket][bclass];
        if (!tbl->valid) tbl = &unigram_tables[bucket];
        if (!tbl->valid) return -1;

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

/* =========================================================================
 * netc_freq_rescale_12_to_10
 *
 * Rescales a 4096-sum frequency table to a 1024-sum frequency table.
 * Algorithm:
 *   1. Count non-zero symbols and scale each freq proportionally.
 *   2. Clamp non-zero symbols to minimum frequency 1.
 *   3. Adjust the largest symbol to absorb any rounding error.
 * ========================================================================= */

int netc_freq_rescale_12_to_10(const netc_freq_table_t *freq12,
                               netc_freq_table_t       *freq10)
{
    if (freq12 == NULL || freq10 == NULL) return -1;

    /* Verify input sums to 4096 */
    uint32_t total12 = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        total12 += freq12->freq[s];
    }
    if (total12 != NETC_TANS_TABLE_SIZE) return -1;

    /* Pass 1: Proportional scaling with rounding, clamped to min 1 for non-zero */
    uint32_t total10 = 0;
    int      largest_sym = -1;
    uint32_t largest_freq12 = 0;

    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq12->freq[s] == 0) {
            freq10->freq[s] = 0;
            continue;
        }
        /* Scale: freq10 = round(freq12 * 1024 / 4096) = round(freq12 / 4) */
        uint32_t scaled = (uint32_t)(((uint64_t)freq12->freq[s] * NETC_TANS_TABLE_SIZE_10
                           + NETC_TANS_TABLE_SIZE / 2) / NETC_TANS_TABLE_SIZE);
        if (scaled == 0) scaled = 1;  /* minimum frequency for non-zero symbols */
        freq10->freq[s] = (uint16_t)scaled;
        total10 += scaled;

        /* Track the largest symbol (by 12-bit freq) for adjustment */
        if (freq12->freq[s] > largest_freq12) {
            largest_freq12 = freq12->freq[s];
            largest_sym = s;
        }
    }

    /* Pass 2: Adjust the largest symbol to hit exactly 1024 */
    if (largest_sym < 0) return -1;  /* no non-zero symbols */

    int32_t diff = (int32_t)NETC_TANS_TABLE_SIZE_10 - (int32_t)total10;
    int32_t new_freq = (int32_t)freq10->freq[largest_sym] + diff;
    if (new_freq < 1) {
        /* Adjustment would make the largest symbol < 1.
         * This can only happen with extreme distributions — redistribute. */
        freq10->freq[largest_sym] = 1;
        total10 = 0;
        for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
            total10 += freq10->freq[s];
        }
        diff = (int32_t)NETC_TANS_TABLE_SIZE_10 - (int32_t)total10;
        for (int s = 0; diff != 0 && s < (int)NETC_TANS_SYMBOLS; s++) {
            if (s == largest_sym || freq10->freq[s] == 0) continue;
            if (diff > 0) {
                freq10->freq[s]++;
                diff--;
            } else if (freq10->freq[s] > 1) {
                freq10->freq[s]--;
                diff++;
            }
        }
    } else {
        freq10->freq[largest_sym] = (uint16_t)new_freq;
    }

    return 0;
}

/* =========================================================================
 * netc_tans_build_10
 *
 * 10-bit variant of netc_tans_build.
 * Uses TABLE_SIZE_10 (1024), TABLE_LOG_10 (10), SPREAD_STEP_10 (643).
 * ========================================================================= */

int netc_tans_build_10(netc_tans_table_10_t *tbl, const netc_freq_table_t *freq) {
    if (tbl == NULL || freq == NULL) return -1;

    /* Validate sum */
    uint32_t total = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        total += freq->freq[s];
    }
    if (total != NETC_TANS_TABLE_SIZE_10) return -1;

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

    /* --- Step 2: Build encode entries per symbol --- */
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        int fl     = floor_log2_u32(f);
        int nb_hi  = (int)NETC_TANS_TABLE_LOG_10 - fl;
        if (nb_hi < 0) nb_hi = 0;
        tbl->encode[s].freq  = (uint16_t)f;
        tbl->encode[s].nb_hi = (uint8_t)nb_hi;
        tbl->encode[s].lower = (uint16_t)(f << (uint32_t)nb_hi);
        tbl->encode[s].cumul = cumul[s];
    }

    /* --- Step 3: Generate spread table using FSE step function ---
     * step = 643. GCD(643, 1024) = 1, so all 1024 positions are visited. */
    uint8_t  spread_sym[NETC_TANS_TABLE_SIZE_10];
    uint16_t rank[NETC_TANS_SYMBOLS];
    memset(rank, 0, sizeof(rank));
    memset(spread_sym, 0, sizeof(spread_sym));

    uint32_t pos = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        for (uint32_t k = 0; k < f; k++) {
            spread_sym[pos] = (uint8_t)s;
            tbl->encode_state[cumul[s] + k] = (uint16_t)(NETC_TANS_TABLE_SIZE_10 + pos);
            pos = (pos + NETC_TANS_SPREAD_STEP_10) & (NETC_TANS_TABLE_SIZE_10 - 1U);
        }
    }

    /* --- Step 4: Build decode table --- */
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) rank[s] = 0;
    (void)spread_sym; /* used implicitly above */

    pos = 0;
    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        if (freq->freq[s] == 0) continue;
        uint32_t f = freq->freq[s];
        for (uint32_t k = 0; k < f; k++) {
            uint32_t X_prev = f + k;
            int nb_j = (int)NETC_TANS_TABLE_LOG_10 - floor_log2_u32(X_prev);
            if (nb_j < 0) nb_j = 0;

            tbl->decode[pos].symbol         = (uint8_t)s;
            tbl->decode[pos].nb_bits        = (uint8_t)nb_j;
            tbl->decode[pos].next_state_base = (uint16_t)(X_prev << (uint32_t)nb_j);

            pos = (pos + NETC_TANS_SPREAD_STEP_10) & (NETC_TANS_TABLE_SIZE_10 - 1U);
        }
    }

    tbl->valid = 1;
    return 0;
}

/* =========================================================================
 * netc_tans_encode_10
 *
 * 10-bit variant of netc_tans_encode.
 * State range: [1024, 2048).
 * Returns final state (initial state for decoder), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode_10(
    const netc_tans_table_10_t *tbl,
    const uint8_t              *src,
    size_t                      src_size,
    netc_bsw_t                 *bsw,
    uint32_t                    initial_state)
{
    if (!tbl || !tbl->valid || !src || !bsw || src_size == 0) return 0;

    uint32_t X = initial_state;
    if (X < NETC_TANS_TABLE_SIZE_10) X = NETC_TANS_TABLE_SIZE_10;

    for (size_t i = src_size; i-- > 0; ) {
        uint8_t sym = src[i];

        const netc_tans_encode_entry_t *e = &tbl->encode[sym];
        uint32_t f     = e->freq;
        uint32_t lower = e->lower;
        int      nb_hi = (int)e->nb_hi;

        if (f == 0) return 0; /* symbol not in table */

        int      nb = (nb_hi == 0 || X >= lower) ? nb_hi : nb_hi - 1;
        uint32_t j  = (X >> (uint32_t)nb) - f;

        if (nb > 0) {
            if (netc_bsw_write(bsw, X & ((1U << (uint32_t)nb) - 1U), nb) != 0)
                return 0;
        }

        X = (uint32_t)tbl->encode_state[(uint32_t)e->cumul + j];
    }

    return X;
}

/* =========================================================================
 * netc_tans_decode_10
 *
 * 10-bit variant of netc_tans_decode.
 * State range: [1024, 2048).
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_10(
    const netc_tans_table_10_t *tbl,
    netc_bsr_t                 *bsr,
    uint8_t                    *dst,
    size_t                      dst_size,
    uint32_t                    initial_state)
{
    if (!tbl || !tbl->valid || !bsr || !dst || dst_size == 0) return -1;

    uint32_t X = initial_state;

    if (X < NETC_TANS_TABLE_SIZE_10 || X >= 2U * NETC_TANS_TABLE_SIZE_10) return -1;

    NETC_PREFETCH(&tbl->decode[X - NETC_TANS_TABLE_SIZE_10]);

    for (size_t i = 0; i < dst_size; i++) {
        uint32_t slot = X - NETC_TANS_TABLE_SIZE_10;
        const netc_tans_decode_entry_t *d = &tbl->decode[slot];

        dst[i] = d->symbol;

        int      nb       = d->nb_bits;
        uint32_t bits_val = 0;
        if (nb > 0) {
            if (netc_bsr_read(bsr, nb, &bits_val) != 0) return -1;
        }

        X = (uint32_t)d->next_state_base + bits_val;

        if (i + 1 < dst_size) {
            NETC_PREFETCH(&tbl->decode[X - NETC_TANS_TABLE_SIZE_10]);
        }
    }

    return 0;
}
