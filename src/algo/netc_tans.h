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

/* Context bucket indices (RFC-001 §6.2) */
#define NETC_CTX_HEADER     0U   /* byte offsets [0..15] */
#define NETC_CTX_SUBHEADER  1U   /* byte offsets [16..63] */
#define NETC_CTX_BODY       2U   /* byte offsets [64..255] */
#define NETC_CTX_TAIL       3U   /* byte offsets [256..65535] */
#define NETC_CTX_COUNT      4U

/* Map a byte offset to its context bucket index */
static NETC_INLINE uint32_t netc_ctx_bucket(uint32_t offset) {
    if (offset < 16U)  return NETC_CTX_HEADER;
    if (offset < 64U)  return NETC_CTX_SUBHEADER;
    if (offset < 256U) return NETC_CTX_BODY;
    return NETC_CTX_TAIL;
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
 * Encode step (given state X ∈ [TABLE_SIZE, 2*TABLE_SIZE)):
 *   f        = freq[sym]
 *   fl       = floor_log2(f)
 *   nb_hi    = encode[sym].nb_hi  (= TABLE_LOG - fl)
 *   nb_lo    = nb_hi - 1          (if f is not a power of 2)
 *   lower    = f << nb_hi         (threshold: X < lower → use nb_lo)
 *   nb       = (X >= lower) ? nb_hi : nb_lo
 *   bits     = X & ((1u << nb) - 1)
 *   j        = (X >> nb) - f      (index into symbol's state table)
 *   new_X    = encode[sym].state_table[cumul + j]  (looked up via full table)
 * ========================================================================= */

typedef struct {
    uint16_t cumul;   /* Cumulative freq before this symbol (start of encode range) */
    uint8_t  nb_hi;   /* TABLE_LOG - floor_log2(freq[sym]) */
    uint8_t  _pad;
} netc_tans_encode_entry_t;

/* =========================================================================
 * Per-bucket tANS table
 *
 * encode_state[TABLE_SIZE]: maps cumul[s]+j → actual slot in spread table
 *   (the k-th occurrence of symbol s is stored at encode_state[cumul[s]+k])
 * ========================================================================= */

typedef struct {
    netc_tans_decode_entry_t decode[NETC_TANS_TABLE_SIZE]; /* 16 KB */
    uint16_t                 encode_state[NETC_TANS_TABLE_SIZE]; /* 8 KB */
    netc_tans_encode_entry_t encode[NETC_TANS_SYMBOLS];    /* 1 KB */
    netc_freq_table_t        freq;                          /* 512 B */
    uint8_t                  valid;  /* 1 if tables are built, 0 otherwise */
    uint8_t                  _pad[3];
} netc_tans_table_t;

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

#endif /* NETC_TANS_H */
