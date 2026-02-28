/**
 * netc_adaptive.h -- Adaptive cross-packet frequency learning.
 *
 * INTERNAL HEADER -- not part of the public API.
 *
 * When NETC_CFG_FLAG_ADAPTIVE is set, both encoder and decoder call
 * netc_adaptive_update() after each packet to accumulate byte
 * frequency statistics. Every NETC_ADAPTIVE_INTERVAL packets,
 * netc_adaptive_tables_rebuild() blends the accumulated frequencies
 * with the dictionary baseline and rebuilds the tANS tables.
 *
 * Synchronization: both sides see the same decompressed bytes, so
 * both call the same update functions in the same order. No explicit
 * sync protocol is needed.
 */

#ifndef NETC_ADAPTIVE_H
#define NETC_ADAPTIVE_H

#include "../core/netc_internal.h"

/**
 * Update frequency accumulators with bytes from a decompressed packet.
 *
 * Called by both encoder (on raw input) and decoder (on reconstructed output)
 * after each successful compress/decompress. The bytes are the ORIGINAL
 * (uncompressed) packet data -- NOT the delta residuals.
 *
 * Also triggers a table rebuild every NETC_ADAPTIVE_INTERVAL packets.
 *
 * @param ctx   Context with NETC_CFG_FLAG_ADAPTIVE set
 * @param data  Decompressed packet bytes
 * @param size  Size of the packet
 */
static NETC_INLINE void netc_adaptive_update(netc_ctx_t *ctx,
                                              const uint8_t *data,
                                              size_t size);

/**
 * Rebuild tANS tables by blending accumulated frequencies with dict baseline.
 *
 * Internal function called by netc_adaptive_update() when the rebuild
 * interval is reached.
 */
void netc_adaptive_tables_rebuild(netc_ctx_t *ctx);

/* ---- Implementation of the inline update ---- */

static NETC_INLINE void netc_adaptive_update(netc_ctx_t *ctx,
                                              const uint8_t *data,
                                              size_t size)
{
    if (!ctx->adapt_freq) return;  /* not adaptive */

    /* Accumulate byte frequencies per-bucket */
    uint32_t *freq = ctx->adapt_freq;   /* [NETC_CTX_COUNT][256] flat */
    uint32_t *total = ctx->adapt_total; /* [NETC_CTX_COUNT] */

    for (size_t i = 0; i < size; i++) {
        uint32_t b = netc_ctx_bucket((uint32_t)i);
        freq[b * 256 + data[i]]++;
        total[b]++;
    }

    /* Check if we should rebuild tables */
    ctx->adapt_pkt_count++;
    if (ctx->adapt_pkt_count >= NETC_ADAPTIVE_INTERVAL) {
        netc_adaptive_tables_rebuild(ctx);
        ctx->adapt_pkt_count = 0;
    }
}

#endif /* NETC_ADAPTIVE_H */
