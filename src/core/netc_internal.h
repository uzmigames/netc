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
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Limits
 * ========================================================================= */

#define NETC_DEFAULT_RING_SIZE  (64u * 1024u)  /* 64 KB */
#define NETC_DEFAULT_ARENA_SIZE (NETC_MAX_PACKET_SIZE * 2u + 64u)  /* ~131 KB */
#define NETC_DICT_MAGIC         0x4E455443U    /* "NETC" */
#define NETC_DICT_VERSION       1U

/* =========================================================================
 * Dictionary internals
 * ========================================================================= */

/**
 * netc_dict_t — trained probability model.
 *
 * Holds per-context-bucket tANS tables built from training data.
 * One table per context bucket (RFC-001 §6.2): HEADER, SUBHEADER, BODY, TAIL.
 */
struct netc_dict {
    uint32_t magic;      /* NETC_DICT_MAGIC — sanity check */
    uint8_t  version;    /* NETC_DICT_VERSION */
    uint8_t  model_id;   /* 1–254; 0 = passthrough only; 255 = reserved */
    uint16_t _pad;

    /* Per-context-bucket tANS tables (Phase 2) */
    netc_tans_table_t tables[NETC_CTX_COUNT];

    uint32_t checksum;   /* CRC32 of all preceding fields */
};

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

    /* --- Sequence counter for stateless delta --- */
    uint8_t            context_seq;   /* Rolling 8-bit counter (RFC-001 §9.1) */

    /* --- Working memory arena (AD-005: zero malloc in hot path) --- */
    uint8_t           *arena;         /* Pre-allocated scratch buffer */
    size_t             arena_size;    /* Arena capacity */

    /* --- Statistics (only valid if NETC_CFG_FLAG_STATS set) --- */
    netc_stats_t       stats;
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

#endif /* NETC_INTERNAL_H */
