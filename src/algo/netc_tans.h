/**
 * netc_tans.h — tANS (FSE) codec internal types and interface.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * tANS parameters (AD-001, AD-003):
 *   TABLE_LOG = 12  →  4096 decode slots × 4 bytes = 16 KB (fits L1 cache)
 *   State range: [TABLE_SIZE, 2×TABLE_SIZE)  i.e. [4096, 8192)
 *
 * The tANS codec operates on a single per-bucket probability table.
 * The compressor selects the bucket based on byte offset (RFC-001 §6.2):
 *   CTX_HEADER    bytes [0..15]
 *   CTX_SUBHEADER bytes [16..63]
 *   CTX_BODY      bytes [64..255]
 *   CTX_TAIL      bytes [256..NETC_MAX_PACKET_SIZE)
 *
 * Table construction uses the FSE spread function (coprime step) to
 * ensure the state machine forms a single globally-traversable chain,
 * as in Zstd FSE.
 */

#ifndef NETC_TANS_H
#define NETC_TANS_H

#include "../util/netc_platform.h"
#include "../util/netc_bitstream.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * tANS parameters
 * ========================================================================= */

#define NETC_TANS_TABLE_LOG  12U
#define NETC_TANS_TABLE_SIZE (1U << NETC_TANS_TABLE_LOG)   /* 4096 */
#define NETC_TANS_SYMBOLS    256U

/* FSE spread step: (TABLE_SIZE/2) + (TABLE_SIZE/8) + 3 = 2563.
 * Coprime with TABLE_SIZE=4096 (2563 is odd → GCD(2563,4096)=1). */
#define NETC_TANS_SPREAD_STEP ((NETC_TANS_TABLE_SIZE >> 1) + (NETC_TANS_TABLE_SIZE >> 3) + 3U)

/* =========================================================================
 * 10-bit tANS parameters — for small packets (<=128B)
 *
 * Smaller table = less per-symbol overhead for infrequent symbols + better
 * L1 cache utilization (7.5 KB total vs 28 KB for 12-bit tables).
 * State range: [1024, 2048) — still fits uint16.
 * ========================================================================= */

#define NETC_TANS_TABLE_LOG_10  10U
#define NETC_TANS_TABLE_SIZE_10 (1U << NETC_TANS_TABLE_LOG_10)  /* 1024 */

/* Spread step: coprime with 1024. (512 + 128 + 3) = 643, GCD(643,1024)=1. */
#define NETC_TANS_SPREAD_STEP_10 643U

/* Context bucket count — 16 fine-grained offset ranges (v0.2+).
 * Finer granularity allows the entropy coder to specialize per byte-offset band,
 * reducing cross-region entropy mixing (e.g. zero-padding vs float fields). */
#define NETC_CTX_COUNT      16U

/* Bigram context class count (v0.5+: 8 trained classes).
 * Each position bucket has NETC_BIGRAM_CTX_COUNT sub-tables, selected by
 * the trained class_map (v5) or prev_byte >> 6 (v4 fallback).
 * Controlled by NETC_CFG_FLAG_BIGRAM / NETC_PKT_FLAG_BIGRAM. */
#define NETC_BIGRAM_CTX_COUNT    8U

/* v4 backward-compat: 4 static classes via prev_byte >> 6. */
#define NETC_BIGRAM_CTX_COUNT_V4 4U

/* Map a previous byte value to its bigram context class.
 * If class_map is non-NULL, uses trained 8-class mapping (v5).
 * Otherwise falls back to static 4-class mapping (v4: prev_byte >> 6). */
static NETC_INLINE uint32_t netc_bigram_class(uint8_t prev_byte,
                                               const uint8_t *class_map) {
    if (class_map) return (uint32_t)class_map[prev_byte];
    return (uint32_t)(prev_byte >> 6);  /* v4 fallback */
}

/* Backward-compat aliases for the four coarse v0.1 names.
 * These map to the new bucket indices that cover the same offset ranges. */
#define NETC_CTX_HEADER     0U   /* offsets [0..7]   — first 8 bytes */
#define NETC_CTX_SUBHEADER  2U   /* offsets [16..23] — first subheader block */
#define NETC_CTX_BODY       6U   /* offsets [64..95] — first body block */
#define NETC_CTX_TAIL      10U   /* offsets [256..383] — first tail block */

/* Map a byte offset to its 16-way context bucket index.
 * Bucket boundaries are chosen to give 8-byte resolution for small packets
 * and progressively coarser resolution for larger offsets. */
static NETC_INLINE uint32_t netc_ctx_bucket(uint32_t offset) {
    if (offset <    8U) return  0U;   /* [0..7]       */
    if (offset <   16U) return  1U;   /* [8..15]      */
    if (offset <   24U) return  2U;   /* [16..23]     */
    if (offset <   32U) return  3U;   /* [24..31]     */
    if (offset <   48U) return  4U;   /* [32..47]     */
    if (offset <   64U) return  5U;   /* [48..63]     */
    if (offset <   96U) return  6U;   /* [64..95]     */
    if (offset <  128U) return  7U;   /* [96..127]    */
    if (offset <  192U) return  8U;   /* [128..191]   */
    if (offset <  256U) return  9U;   /* [192..255]   */
    if (offset <  384U) return 10U;   /* [256..383]   */
    if (offset <  512U) return 11U;   /* [384..511]   */
    if (offset < 1024U) return 12U;   /* [512..1023]  */
    if (offset < 4096U) return 13U;   /* [1024..4095] */
    if (offset <16384U) return 14U;   /* [4096..16383]*/
    return 15U;                        /* [16384..65535]*/
}

/* =========================================================================
 * Normalized frequency table
 *
 * freq[s] = normalized count for symbol s, summing to TABLE_SIZE.
 * Symbols with freq[s] == 0 are not present in the table.
 * ========================================================================= */

typedef struct {
    uint16_t freq[NETC_TANS_SYMBOLS];  /* normalized, sums to TABLE_SIZE */
} netc_freq_table_t;

/* =========================================================================
 * tANS decode table entry — one entry per table slot
 *
 * Decode per slot (FSE spread table):
 *   slot       = X - TABLE_SIZE  (X = current state ∈ [TABLE_SIZE, 2*TABLE_SIZE))
 *   sym        = decode[slot].symbol
 *   nb_bits    = decode[slot].nb_bits
 *   bits       = read(nb_bits) from bitstream
 *   X_prev     = decode[slot].next_state_base + bits
 * ========================================================================= */

typedef struct {
    uint8_t  symbol;          /* Decoded symbol */
    uint8_t  nb_bits;         /* Number of bits to read from bitstream */
    uint16_t next_state_base; /* Base for previous-state reconstruction */
} netc_tans_decode_entry_t;

NETC_STATIC_ASSERT(sizeof(netc_tans_decode_entry_t) == 4,
                   "tANS decode entry must be 4 bytes for L1 cache fit");

/* =========================================================================
 * tANS encode table entry — one entry per symbol
 *
 * Encode step (given state X ∈ [TABLE_SIZE, 2*TABLE_SIZE), symbol s):
 *   freq     = encode[sym].freq   (normalized frequency)
 *   nb_hi    = encode[sym].nb_hi  (= TABLE_LOG - floor_log2(freq))
 *   lower    = encode[sym].lower  (= freq << nb_hi, pre-computed threshold)
 *   nb       = (X >= lower) ? nb_hi : nb_hi - 1
 *   bits     = X & ((1u << nb) - 1)
 *   j        = (X >> nb) - freq
 *   new_X    = encode_state[encode[sym].cumul + j]  (stores TABLE_SIZE+slot)
 *
 * All fields needed for normalization live in one 8-byte struct so a single
 * cache-line fetch covers freq, nb_hi, lower, and cumul — eliminating the
 * separate netc_freq_table_t lookup in the hot path.
 * ========================================================================= */

typedef struct {
    uint16_t freq;    /* Normalized frequency (mirrors freq_table.freq[s]) */
    uint16_t lower;   /* Pre-computed: freq << nb_hi (normalization threshold) */
    uint16_t cumul;   /* Cumulative freq before this symbol (encode_state base) */
    uint8_t  nb_hi;   /* TABLE_LOG - floor_log2(freq) */
    uint8_t  _pad;
} netc_tans_encode_entry_t;  /* 8 bytes — 8 entries per 64-byte cache line */

/* =========================================================================
 * Per-bucket tANS table
 *
 * encode_state[TABLE_SIZE]: maps cumul[s]+j → complete next state X.
 *   Stores TABLE_SIZE + slot directly so the hot path assigns X without add.
 *   (the k-th occurrence of symbol s is at encode_state[cumul[s]+k])
 * ========================================================================= */

typedef struct {
    netc_tans_decode_entry_t decode[NETC_TANS_TABLE_SIZE]; /* 16 KB */
    uint16_t                 encode_state[NETC_TANS_TABLE_SIZE]; /* 8 KB — stores TABLE_SIZE+slot */
    netc_tans_encode_entry_t encode[NETC_TANS_SYMBOLS];    /* 2 KB (8B per entry) */
    netc_freq_table_t        freq;                          /* 512 B — kept for dict serialization */
    uint8_t                  valid;  /* 1 if tables are built, 0 otherwise */
    uint8_t                  _pad[3];
} netc_tans_table_t;

/* =========================================================================
 * Per-bucket 10-bit tANS table (small-packet optimization)
 *
 * Identical structure to netc_tans_table_t but with 1024-entry tables.
 * Total footprint: ~7.5 KB vs ~28 KB for 12-bit tables.
 * ========================================================================= */

typedef struct {
    netc_tans_decode_entry_t decode[NETC_TANS_TABLE_SIZE_10]; /* 4 KB */
    uint16_t                 encode_state[NETC_TANS_TABLE_SIZE_10]; /* 2 KB */
    netc_tans_encode_entry_t encode[NETC_TANS_SYMBOLS];        /* 2 KB */
    netc_freq_table_t        freq;                              /* 512 B — normalized to 1024 */
    uint8_t                  valid;
    uint8_t                  _pad[3];
} netc_tans_table_10_t;

/* =========================================================================
 * tANS table builder
 *
 * Builds encode and decode tables from a normalized frequency table.
 * freq must sum to exactly TABLE_SIZE (4096).
 * Uses FSE spread function for correct global state-chain traversal.
 * Returns 0 on success, -1 on invalid input.
 * ========================================================================= */

int netc_tans_build(netc_tans_table_t *tbl, const netc_freq_table_t *freq);

/* =========================================================================
 * tANS encoder
 *
 * Encodes src[0..src_size) into the bitstream writer (in reverse order).
 * Returns final ANS state (needed for decoder to start), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode(
    const netc_tans_table_t *tbl,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                 initial_state
);

/* =========================================================================
 * tANS decoder
 *
 * Decodes dst_size symbols into dst.
 * initial_state: the final encoder state (stored in the packet header).
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode(
    const netc_tans_table_t *tbl,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state
);

/* =========================================================================
 * Dual-interleaved tANS encoder (x2)
 *
 * Encodes src[0..src_size) using two independent ANS states, exposing
 * instruction-level parallelism to the CPU. Requires src_size >= 2.
 * Emits bits into bsw; returns the two final states in *out_state0/1.
 * Returns 0 on success, -1 on error.
 * ========================================================================= */

int netc_tans_encode_x2(
    const netc_tans_table_t *tbl,
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                *out_state0,
    uint32_t                *out_state1
);

/* =========================================================================
 * Dual-interleaved tANS decoder (x2)
 *
 * Decodes dst_size symbols encoded by netc_tans_encode_x2.
 * Requires dst_size >= 2 and both initial states in [TABLE_SIZE, 2*TABLE_SIZE).
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_x2(
    const netc_tans_table_t *tbl,
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state0,
    uint32_t                 initial_state1
);

/* =========================================================================
 * Per-position context-adaptive tANS encoder (PCTX)
 *
 * Encodes src[0..src_size) in a SINGLE ANS stream, switching the
 * probability table per byte offset: tables[netc_ctx_bucket(offset)].
 * This gives per-position entropy specialization with ZERO descriptor
 * overhead compared to MREG's per-region streams.
 *
 * Returns final state (initial state for decoder), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode_pctx(
    const netc_tans_table_t *tables,   /* array of NETC_CTX_COUNT tables */
    const uint8_t           *src,
    size_t                   src_size,
    netc_bsw_t              *bsw,
    uint32_t                 initial_state
);

/* =========================================================================
 * Per-position context-adaptive tANS decoder (PCTX)
 *
 * Decodes dst_size symbols, switching tables per byte offset.
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_pctx(
    const netc_tans_table_t *tables,   /* array of NETC_CTX_COUNT tables */
    netc_bsr_t              *bsr,
    uint8_t                 *dst,
    size_t                   dst_size,
    uint32_t                 initial_state
);

/* =========================================================================
 * 10-bit tANS table builder
 *
 * Builds encode and decode tables from a normalized frequency table.
 * freq must sum to exactly NETC_TANS_TABLE_SIZE_10 (1024).
 * Uses FSE spread function with NETC_TANS_SPREAD_STEP_10 (643).
 * Returns 0 on success, -1 on invalid input.
 * ========================================================================= */

int netc_tans_build_10(netc_tans_table_10_t *tbl, const netc_freq_table_t *freq);

/* =========================================================================
 * 10-bit tANS encoder
 *
 * Encodes src[0..src_size) into the bitstream writer (in reverse order).
 * State range: [1024, 2048).
 * Returns final ANS state (needed for decoder to start), or 0 on error.
 * ========================================================================= */

uint32_t netc_tans_encode_10(
    const netc_tans_table_10_t *tbl,
    const uint8_t              *src,
    size_t                      src_size,
    netc_bsw_t                 *bsw,
    uint32_t                    initial_state
);

/* =========================================================================
 * 10-bit tANS decoder
 *
 * Decodes dst_size symbols into dst.
 * initial_state: the final encoder state (stored in the packet header).
 * State range: [1024, 2048).
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

int netc_tans_decode_10(
    const netc_tans_table_10_t *tbl,
    netc_bsr_t                 *bsr,
    uint8_t                    *dst,
    size_t                      dst_size,
    uint32_t                    initial_state
);

/* =========================================================================
 * Frequency rescaling: 12-bit (4096-sum) → 10-bit (1024-sum)
 *
 * Rescales a frequency table normalized to 4096 down to 1024.
 * Ensures minimum frequency of 1 for all non-zero symbols.
 * Adjusts the largest symbol to hit exactly 1024.
 * Returns 0 on success, -1 on invalid input.
 * ========================================================================= */

int netc_freq_rescale_12_to_10(const netc_freq_table_t *freq12,
                               netc_freq_table_t       *freq10);

#endif /* NETC_TANS_H */
