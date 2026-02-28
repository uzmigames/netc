/**
 * netc_internal.h — Internal types and structures shared across core modules.
 *
 * INTERNAL HEADER — not part of the public API.
 */

#ifndef NETC_INTERNAL_H
#define NETC_INTERNAL_H

#include "../../include/netc.h"
#include "../util/netc_platform.h"
#include "../algo/netc_tans.h"
#include "../algo/netc_delta.h"
#include "../algo/netc_lzp.h"
#include "../simd/netc_simd.h"
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Limits
 * ========================================================================= */

#define NETC_DEFAULT_RING_SIZE  (64u * 1024u)  /* 64 KB */
#define NETC_DEFAULT_ARENA_SIZE (NETC_MAX_PACKET_SIZE * 2u + 64u)  /* ~131 KB */
#define NETC_DICT_MAGIC         0x4E455443U    /* "NETC" */
#define NETC_DICT_VERSION       5U             /* v0.5: 8-class trained bigram quantization */
#define NETC_DICT_VERSION_V4    4U             /* v0.4: LZP hash-prediction table (backward compat) */

/* Adaptive mode: rebuild interval and blending parameters */
#define NETC_ADAPTIVE_INTERVAL   128U   /* Rebuild tables every N packets */
#define NETC_ADAPTIVE_ALPHA_NUM  3U     /* Blend ratio: alpha = 3/4 (accumulated) */
#define NETC_ADAPTIVE_ALPHA_DEN  4U     /* Blend ratio: (1-alpha) = 1/4 (dict baseline) */

/* =========================================================================
 * Dictionary internals
 * ========================================================================= */

/**
 * netc_dict_t — trained probability model.
 *
 * v0.2: 16 fine-grained context buckets (NETC_CTX_COUNT=16) replacing the
 * original 4 coarse buckets. Each bucket covers a contiguous byte-offset band.
 * The ctx_count field makes the blob format self-describing.
 */
struct netc_dict {
    uint32_t magic;      /* NETC_DICT_MAGIC — sanity check */
    uint8_t  version;    /* NETC_DICT_VERSION (= 5) */
    uint8_t  model_id;   /* 1–254; 0 = passthrough only; 255 = reserved */
    uint8_t  ctx_count;  /* Number of context buckets stored (= NETC_CTX_COUNT) */
    uint8_t  dict_flags; /* NETC_DICT_FLAG_* bitmask (was _pad in v3) */

    /* Per-context-bucket tANS tables — 16 tables in v0.2+ */
    netc_tans_table_t tables[NETC_CTX_COUNT];

    /* Per-bucket bigram sub-tables (v0.3+).
     * bigram_tables[bucket][class] is the tANS table used when the previous byte
     * maps to bigram class `class` (via netc_bigram_class(prev_byte, class_map)).
     * v4 dicts: 4 classes per bucket (static prev>>6).
     * v5 dicts: 8 classes per bucket (trained class_map).
     * Only populated when trained with NETC_CFG_FLAG_BIGRAM. */
    netc_tans_table_t bigram_tables[NETC_CTX_COUNT][NETC_BIGRAM_CTX_COUNT];

    /* Trained bigram class map (v0.5+): maps each byte value (0-255) to class 0-7.
     * For v4 dicts loaded into v5 code, this is built from prev_byte >> 6. */
    uint8_t  bigram_class_map[256];

    /* Number of bigram classes actually in use: 4 for v4 dicts, 8 for v5 dicts. */
    uint8_t  bigram_class_count;

    /* LZP hash table (v0.4+, optional).
     * Maps 3-byte context hashes to predicted next bytes.
     * NULL when no LZP model is present (v3 backward compat).
     * Allocated as a separate block of NETC_LZP_HT_SIZE entries. */
    netc_lzp_entry_t *lzp_table;

    uint32_t checksum;   /* CRC32 of all preceding fields */
};

/* Dictionary flags (dict_flags field) */
#define NETC_DICT_FLAG_LZP   0x01U  /* LZP table is present in blob */

/* =========================================================================
 * Context internals
 * ========================================================================= */

/**
 * netc_ctx_t — per-connection compression context.
 *
 * One per logical connection per thread. Not thread-safe.
 */
struct netc_ctx {
    /* --- Configuration (set at creation, read-only in hot path) --- */
    const netc_dict_t *dict;          /* Shared read-only dictionary (may be NULL) */
    uint32_t           flags;         /* NETC_CFG_FLAG_* bitmask */
    uint8_t            compression_level;
    uint8_t            simd_level;

    /* --- Stateful mode ring buffer --- */
    uint8_t           *ring;          /* Ring buffer for history (NULL in stateless) */
    uint32_t           ring_size;     /* Allocated ring buffer size */
    uint32_t           ring_pos;      /* Current write position (wraps) */

    /* --- SIMD dispatch table (set at ctx_create, read-only in hot path) --- */
    netc_simd_ops_t    simd_ops;      /* Best available bulk operation implementations */

    /* --- Delta prediction state (stateful mode) --- */
    uint8_t           *prev_pkt;      /* Copy of last packet before delta (for encoder/decoder symmetry) */
    size_t             prev_pkt_size; /* Size of bytes valid in prev_pkt (0 = no prior packet) */

    /* --- Sequence counter for stateless delta --- */
    uint8_t            context_seq;   /* Rolling 8-bit counter (RFC-001 §9.1) */

    /* --- Working memory arena (AD-005: zero malloc in hot path) --- */
    uint8_t           *arena;         /* Pre-allocated scratch buffer */
    size_t             arena_size;    /* Arena capacity */

    /* --- Statistics (only valid if NETC_CFG_FLAG_STATS set) --- */
    netc_stats_t       stats;

    /* --- Adaptive mode state (Phase 1: frequency tracking + table rebuild) --- */
    uint32_t          *adapt_freq;       /* [NETC_CTX_COUNT][256] frequency accumulators (NULL if not adaptive) */
    uint32_t          *adapt_total;      /* [NETC_CTX_COUNT] total byte count per bucket */
    netc_tans_table_t *adapt_tables;     /* [NETC_CTX_COUNT] mutable tANS tables (NULL if not adaptive) */
    uint32_t           adapt_pkt_count;  /* Packets processed since last table rebuild */
};

/* =========================================================================
 * Packet header layout helpers — RFC-001 §9.1
 *
 * Offset  Size  Field
 *  0       2    original_size   (uint16 LE)
 *  2       2    compressed_size (uint16 LE)
 *  4       1    flags           (NETC_PKT_FLAG_*)
 *  5       1    algorithm       (NETC_ALG_*)
 *  6       1    model_id
 *  7       1    context_seq
 *  8       N    payload
 * ========================================================================= */

typedef struct {
    uint16_t original_size;
    uint16_t compressed_size;
    uint8_t  flags;
    uint8_t  algorithm;
    uint8_t  model_id;
    uint8_t  context_seq;
} netc_pkt_header_t;

NETC_STATIC_ASSERT(sizeof(netc_pkt_header_t) == NETC_HEADER_SIZE,
                   "netc_pkt_header_t must be exactly NETC_HEADER_SIZE bytes");

/** Write a packet header to a raw byte buffer (little-endian). */
static NETC_INLINE void netc_hdr_write(void *dst, const netc_pkt_header_t *h) {
    uint8_t *b = (uint8_t *)dst;
    netc_write_u16_le(b + 0, h->original_size);
    netc_write_u16_le(b + 2, h->compressed_size);
    b[4] = h->flags;
    b[5] = h->algorithm;
    b[6] = h->model_id;
    b[7] = h->context_seq;
}

/** Read a packet header from a raw byte buffer (little-endian). */
static NETC_INLINE void netc_hdr_read(const void *src, netc_pkt_header_t *h) {
    const uint8_t *b = (const uint8_t *)src;
    h->original_size   = netc_read_u16_le(b + 0);
    h->compressed_size = netc_read_u16_le(b + 2);
    h->flags           = b[4];
    h->algorithm       = b[5];
    h->model_id        = b[6];
    h->context_seq     = b[7];
}

/* =========================================================================
 * Compact packet header — 2 or 4 bytes (opt-in via NETC_CFG_FLAG_COMPACT_HDR)
 *
 * Byte 0:  PACKET_TYPE (flags + algorithm + bucket packed into one byte)
 * Byte 1:  [E][SSSSSSS]
 *           E=0: original_size = SSSSSSS (0-127).  Header = 2 bytes.
 *           E=1: bytes 2-3 = original_size u16 LE.  Header = 4 bytes.
 *
 * Eliminated fields (derived at runtime):
 *   compressed_size = src_size - header_size
 *   model_id        = ctx->dict->model_id
 *   context_seq     = ctx->context_seq
 * ========================================================================= */

/* --- Packet type encoding (byte 0) ---
 *
 * Non-bucketed (0x00-0x0F):
 *   0x00 PASSTHRU             0x08 TANS_MREG
 *   0x01 PASSTHRU+LZ77        0x09 TANS_MREG+DELTA
 *   0x02 PASSTHRU+LZ77+DELTA  0x0A TANS_MREG+X2
 *   0x03 PASSTHRU+RLE         0x0B TANS_MREG+X2+DELTA
 *   0x04 TANS_PCTX            0x0C TANS_MREG+BIGRAM
 *   0x05 TANS_PCTX+DELTA      0x0D TANS_MREG+BIGRAM+DELTA
 *   0x06 TANS_PCTX+LZP        0x0E LZ77X
 *   0x07 TANS_PCTX+LZP+DELTA  0x0F reserved
 *
 * Bucketed (base + bucket[0..15]):
 *   0x10-0x1F TANS              0x50-0x5F TANS+X2
 *   0x20-0x2F TANS+DELTA        0x60-0x6F TANS+X2+DELTA
 *   0x30-0x3F TANS+BIGRAM       0x70-0x7F LZP
 *   0x40-0x4F TANS+BIGRAM+DELTA 0x80-0x8F LZP+DELTA
 *
 *   0xFF = invalid / legacy sentinel
 */

typedef struct {
    uint8_t flags;
    uint8_t algorithm;
} netc_pkt_type_entry_t;

/* Decode table: pkt_type byte → (flags, algorithm).
 * Entries with flags==0xFF are invalid. */
static const netc_pkt_type_entry_t netc_pkt_type_table[256] = {
    /* 0x00-0x03: Passthrough variants */
    [0x00] = { NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DICT_ID, NETC_ALG_PASSTHRU },
    [0x01] = { NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_PASSTHRU },
    [0x02] = { NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_PASSTHRU },
    [0x03] = { NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_RLE | NETC_PKT_FLAG_DICT_ID, NETC_ALG_PASSTHRU },

    /* 0x04-0x07: PCTX variants */
    [0x04] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX },
    [0x05] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX },
    [0x06] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX | 0x10u },
    [0x07] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX | 0x10u },

    /* 0x08-0x0D: MREG variants */
    [0x08] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },
    [0x09] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },
    [0x0A] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },
    [0x0B] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },
    [0x0C] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },
    [0x0D] = { NETC_PKT_FLAG_MREG | NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS },

    /* 0x0E: LZ77X */
    [0x0E] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZ77X },

    /* 0x10-0x1F: TANS + bucket 0-15 */
    [0x10] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x11] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x12] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x13] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x14] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x15] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x16] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x17] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x18] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x19] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x1A] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x1B] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x1C] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x1D] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x1E] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x1F] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x20-0x2F: TANS + DELTA + bucket 0-15 */
    [0x20] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x21] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x22] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x23] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x24] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x25] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x26] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x27] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x28] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x29] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x2A] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x2B] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x2C] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x2D] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x2E] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x2F] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x30-0x3F: TANS + BIGRAM + bucket 0-15 */
    [0x30] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x31] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x32] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x33] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x34] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x35] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x36] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x37] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x38] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x39] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x3A] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x3B] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x3C] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x3D] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x3E] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x3F] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x40-0x4F: TANS + BIGRAM + DELTA + bucket 0-15 */
    [0x40] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x41] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x42] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x43] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x44] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x45] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x46] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x47] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x48] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x49] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x4A] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x4B] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x4C] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x4D] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x4E] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x4F] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x50-0x5F: TANS + X2 + bucket 0-15 */
    [0x50] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x51] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x52] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x53] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x54] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x55] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x56] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x57] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x58] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x59] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x5A] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x5B] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x5C] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x5D] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x5E] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x5F] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x60-0x6F: TANS + X2 + DELTA + bucket 0-15 */
    [0x60] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (0u<<4) },
    [0x61] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (1u<<4) },
    [0x62] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (2u<<4) },
    [0x63] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (3u<<4) },
    [0x64] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (4u<<4) },
    [0x65] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (5u<<4) },
    [0x66] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (6u<<4) },
    [0x67] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (7u<<4) },
    [0x68] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (8u<<4) },
    [0x69] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (9u<<4) },
    [0x6A] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (10u<<4) },
    [0x6B] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (11u<<4) },
    [0x6C] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (12u<<4) },
    [0x6D] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (13u<<4) },
    [0x6E] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (14u<<4) },
    [0x6F] = { NETC_PKT_FLAG_X2 | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS | (15u<<4) },

    /* 0x70-0x7F: LZP + bucket 0-15 */
    [0x70] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (0u<<4) },
    [0x71] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (1u<<4) },
    [0x72] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (2u<<4) },
    [0x73] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (3u<<4) },
    [0x74] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (4u<<4) },
    [0x75] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (5u<<4) },
    [0x76] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (6u<<4) },
    [0x77] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (7u<<4) },
    [0x78] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (8u<<4) },
    [0x79] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (9u<<4) },
    [0x7A] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (10u<<4) },
    [0x7B] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (11u<<4) },
    [0x7C] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (12u<<4) },
    [0x7D] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (13u<<4) },
    [0x7E] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (14u<<4) },
    [0x7F] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (15u<<4) },

    /* 0x80-0x8F: LZP + DELTA + bucket 0-15 */
    [0x80] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (0u<<4) },
    [0x81] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (1u<<4) },
    [0x82] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (2u<<4) },
    [0x83] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (3u<<4) },
    [0x84] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (4u<<4) },
    [0x85] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (5u<<4) },
    [0x86] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (6u<<4) },
    [0x87] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (7u<<4) },
    [0x88] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (8u<<4) },
    [0x89] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (9u<<4) },
    [0x8A] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (10u<<4) },
    [0x8B] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (11u<<4) },
    [0x8C] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (12u<<4) },
    [0x8D] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (13u<<4) },
    [0x8E] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (14u<<4) },
    [0x8F] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (15u<<4) },

    /* 0x90-0x9F: LZP + BIGRAM + bucket 0-15 */
    [0x90] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (0u<<4) },
    [0x91] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (1u<<4) },
    [0x92] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (2u<<4) },
    [0x93] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (3u<<4) },
    [0x94] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (4u<<4) },
    [0x95] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (5u<<4) },
    [0x96] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (6u<<4) },
    [0x97] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (7u<<4) },
    [0x98] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (8u<<4) },
    [0x99] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (9u<<4) },
    [0x9A] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (10u<<4) },
    [0x9B] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (11u<<4) },
    [0x9C] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (12u<<4) },
    [0x9D] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (13u<<4) },
    [0x9E] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (14u<<4) },
    [0x9F] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (15u<<4) },

    /* 0xA0-0xAF: LZP + BIGRAM + DELTA + bucket 0-15 */
    [0xA0] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (0u<<4) },
    [0xA1] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (1u<<4) },
    [0xA2] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (2u<<4) },
    [0xA3] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (3u<<4) },
    [0xA4] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (4u<<4) },
    [0xA5] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (5u<<4) },
    [0xA6] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (6u<<4) },
    [0xA7] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (7u<<4) },
    [0xA8] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (8u<<4) },
    [0xA9] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (9u<<4) },
    [0xAA] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (10u<<4) },
    [0xAB] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (11u<<4) },
    [0xAC] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (12u<<4) },
    [0xAD] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (13u<<4) },
    [0xAE] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (14u<<4) },
    [0xAF] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_LZP | (15u<<4) },

    /* 0xB0-0xBF: TANS_10BIT + bucket 0-15 (10-bit tANS, small-packet optimization) */
    [0xB0] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (0u<<4) },
    [0xB1] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (1u<<4) },
    [0xB2] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (2u<<4) },
    [0xB3] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (3u<<4) },
    [0xB4] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (4u<<4) },
    [0xB5] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (5u<<4) },
    [0xB6] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (6u<<4) },
    [0xB7] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (7u<<4) },
    [0xB8] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (8u<<4) },
    [0xB9] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (9u<<4) },
    [0xBA] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (10u<<4) },
    [0xBB] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (11u<<4) },
    [0xBC] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (12u<<4) },
    [0xBD] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (13u<<4) },
    [0xBE] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (14u<<4) },
    [0xBF] = { NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (15u<<4) },

    /* 0xC0-0xCF: TANS_10BIT + DELTA + bucket 0-15 */
    [0xC0] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (0u<<4) },
    [0xC1] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (1u<<4) },
    [0xC2] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (2u<<4) },
    [0xC3] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (3u<<4) },
    [0xC4] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (4u<<4) },
    [0xC5] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (5u<<4) },
    [0xC6] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (6u<<4) },
    [0xC7] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (7u<<4) },
    [0xC8] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (8u<<4) },
    [0xC9] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (9u<<4) },
    [0xCA] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (10u<<4) },
    [0xCB] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (11u<<4) },
    [0xCC] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (12u<<4) },
    [0xCD] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (13u<<4) },
    [0xCE] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (14u<<4) },
    [0xCF] = { NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_10 | (15u<<4) },

    /* 0xD0-0xD3: PCTX + BIGRAM variants */
    [0xD0] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX },
    [0xD1] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX },
    [0xD2] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX | 0x10u },
    [0xD3] = { NETC_PKT_FLAG_BIGRAM | NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_DICT_ID, NETC_ALG_TANS_PCTX | 0x10u },

    /* 0xD4-0xFE: reserved (zero-initialized → flags=0, algorithm=0 → invalid) */
    /* 0xFF: legacy sentinel */
    [0xFF] = { 0xFF, 0xFF },
};

/** Encode flags + algorithm into a compact packet type byte.
 *  Returns 0xFF on invalid/unrepresentable combination. */
static NETC_INLINE uint8_t netc_compact_type_encode(uint8_t flags, uint8_t algorithm)
{
    uint8_t alg_lo  = algorithm & 0x0Fu;
    uint8_t bucket  = (algorithm >> 4) & 0x0Fu;
    uint8_t delta   = (flags & NETC_PKT_FLAG_DELTA)  ? 1u : 0u;
    uint8_t bigram  = (flags & NETC_PKT_FLAG_BIGRAM) ? 1u : 0u;
    uint8_t x2      = (flags & NETC_PKT_FLAG_X2)     ? 1u : 0u;

    /* Passthrough */
    if (flags & NETC_PKT_FLAG_PASSTHRU) {
        if (flags & NETC_PKT_FLAG_LZ77)  return (uint8_t)(0x01u + delta);
        if (flags & NETC_PKT_FLAG_RLE)   return 0x03u;
        return 0x00u;
    }

    /* PCTX */
    if (alg_lo == NETC_ALG_TANS_PCTX) {
        uint8_t lzp = (bucket != 0) ? 1u : 0u; /* high nibble signals LZP */
        if (bigram) return (uint8_t)(0xD0u + delta + lzp * 2u);
        return (uint8_t)(0x04u + delta + lzp * 2u);
    }

    /* LZ77X */
    if (alg_lo == NETC_ALG_LZ77X) return 0x0Eu;

    /* MREG */
    if (flags & NETC_PKT_FLAG_MREG) {
        if (bigram) return (uint8_t)(0x0Cu + delta);
        if (x2)     return (uint8_t)(0x0Au + delta);
        return (uint8_t)(0x08u + delta);
    }

    /* Single-region TANS with bucket */
    if (alg_lo == NETC_ALG_TANS) {
        uint8_t base;
        if      (bigram && delta) base = 0x40u;
        else if (bigram)          base = 0x30u;
        else if (x2 && delta)     base = 0x60u;
        else if (x2)              base = 0x50u;
        else if (delta)           base = 0x20u;
        else                      base = 0x10u;
        return (uint8_t)(base + bucket);
    }

    /* LZP with bucket */
    if (alg_lo == NETC_ALG_LZP) {
        uint8_t base;
        if      (bigram && delta) base = 0xA0u;
        else if (bigram)          base = 0x90u;
        else if (delta)           base = 0x80u;
        else                      base = 0x70u;
        return (uint8_t)(base + bucket);
    }

    /* 10-bit tANS with bucket (no bigram/x2 variants — small packets only) */
    if (alg_lo == NETC_ALG_TANS_10) {
        uint8_t base = delta ? 0xC0u : 0xB0u;
        return (uint8_t)(base + bucket);
    }

    return 0xFFu; /* unrepresentable */
}

/** Write a compact header. Returns bytes written (2 or 4). */
static NETC_INLINE size_t netc_hdr_write_compact(void *dst,
                                                   uint8_t pkt_type,
                                                   uint16_t original_size)
{
    uint8_t *b = (uint8_t *)dst;
    b[0] = pkt_type;
    if (original_size <= 127u) {
        b[1] = (uint8_t)original_size; /* bit 7 = 0 */
        return 2u;
    }
    b[1] = 0x80u; /* extension marker */
    netc_write_u16_le(b + 2, original_size);
    return 4u;
}

/** Read a compact header. Returns bytes consumed (2 or 4), or 0 on error.
 *  Fills hdr->original_size, hdr->flags, hdr->algorithm from wire bytes.
 *  Caller must fill compressed_size, model_id, context_seq separately. */
static NETC_INLINE size_t netc_hdr_read_compact(const void *src,
                                                  size_t src_size,
                                                  netc_pkt_header_t *hdr)
{
    if (NETC_UNLIKELY(src_size < 2u)) return 0u;
    const uint8_t *b = (const uint8_t *)src;

    uint8_t pkt_type = b[0];
    const netc_pkt_type_entry_t *e = &netc_pkt_type_table[pkt_type];
    /* Invalid type: both flags==0 and algorithm==0 means unused slot,
     * except 0xFF which is the explicit sentinel. */
    if (NETC_UNLIKELY(pkt_type == 0xFFu || (e->flags == 0u && e->algorithm == 0u)))
        return 0u;

    hdr->flags     = e->flags;
    hdr->algorithm = e->algorithm;

    if (!(b[1] & 0x80u)) {
        /* Short form: original_size in 7 bits */
        hdr->original_size = (uint16_t)(b[1] & 0x7Fu);
        return 2u;
    }
    /* Long form: 16-bit original_size at bytes 2-3 */
    if (NETC_UNLIKELY(src_size < 4u)) return 0u;
    hdr->original_size = netc_read_u16_le(b + 2);
    return 4u;
}

/** Unified header emit: writes compact or legacy header.
 *  Returns the number of header bytes written. */
static NETC_INLINE size_t netc_hdr_emit(void *dst,
                                          const netc_pkt_header_t *h,
                                          int compact)
{
    if (compact) {
        uint8_t ptype = netc_compact_type_encode(h->flags, h->algorithm);
        return netc_hdr_write_compact(dst, ptype, h->original_size);
    }
    netc_hdr_write(dst, h);
    return NETC_HEADER_SIZE;
}

/* Get the tANS tables to use (adaptive or dict static).
 * When adaptive mode is active and adapt_tables is populated, returns the
 * mutable adaptive tables. Otherwise returns the frozen dict tables. */
static NETC_INLINE const netc_tans_table_t *netc_get_tables(const netc_ctx_t *ctx) {
    return (ctx->adapt_tables != NULL) ? ctx->adapt_tables : ctx->dict->tables;
}

#endif /* NETC_INTERNAL_H */
