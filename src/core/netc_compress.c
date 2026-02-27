/**
 * netc_compress.c — Compression entry point.
 *
 * Phase 2: tANS compression with passthrough fallback (AD-006).
 *   - Validates all arguments.
 *   - If a dictionary with valid tANS tables is present, attempts tANS encoding
 *     per context bucket (RFC-001 §6.2).
 *   - Falls back to passthrough if compressed_size >= original_size (AD-006).
 *   - Updates statistics if NETC_CFG_FLAG_STATS is set.
 *
 * Packet layout for tANS (algorithm = NETC_ALG_TANS):
 *   [header  8 bytes ]
 *   [initial_state 4 bytes  LE — encoder final state for decoder init]
 *   [bitstream payload — variable length]
 */

#include "netc_internal.h"
#include "../algo/netc_tans.h"
#include "../util/netc_bitstream.h"
#include <string.h>

/* =========================================================================
 * Internal: emit a passthrough packet
 * ========================================================================= */

static netc_result_t emit_passthrough(
    netc_ctx_t        *ctx,
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size,
    uint8_t            context_seq)
{
    size_t out_size = NETC_HEADER_SIZE + src_size;

    if (NETC_UNLIKELY(dst_cap < out_size)) {
        return NETC_ERR_BUF_SMALL;
    }

    netc_pkt_header_t hdr;
    hdr.original_size   = (uint16_t)src_size;
    hdr.compressed_size = (uint16_t)src_size;
    hdr.flags           = NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DICT_ID;
    hdr.algorithm       = NETC_ALG_PASSTHRU;
    hdr.model_id        = (dict != NULL) ? dict->model_id : 0;
    hdr.context_seq     = context_seq;

    uint8_t *out = (uint8_t *)dst;
    netc_hdr_write(out, &hdr);
    memcpy(out + NETC_HEADER_SIZE, src, src_size);

    *dst_size = out_size;

    if (ctx != NULL && (ctx->flags & NETC_CFG_FLAG_STATS)) {
        ctx->stats.packets_compressed++;
        ctx->stats.bytes_in  += src_size;
        ctx->stats.bytes_out += out_size;
        ctx->stats.passthrough_count++;
    }

    return NETC_OK;
}

/* =========================================================================
 * Internal: attempt tANS compression
 *
 * The tANS codec encodes all bytes with the same per-bucket table.
 * For simplicity in v0.1, we use a single table per packet (the bucket
 * matching the majority of bytes — CTX_BODY for packets > 64 bytes).
 * Full per-offset bucket selection is deferred to Phase 3 (delta pass).
 *
 * Wire format after the 8-byte header:
 *   [4] initial_state (uint32 LE)
 *   [N] bitstream payload
 *
 * Returns NETC_OK on success (and sets *dst_size).
 * Returns NETC_ERR_BUF_SMALL if compressed output doesn't fit.
 * Returns a negative error if tANS fails (caller falls back to passthrough).
 * ========================================================================= */

static int try_tans_compress(
    const netc_dict_t *dict,
    const uint8_t     *src,
    size_t             src_size,
    uint8_t           *dst,         /* points past the 8-byte header */
    size_t             dst_payload_cap,
    size_t            *compressed_payload_size)
{
    /* Select context bucket based on dominant packet region.
     * For packets ≤ 15 bytes → HEADER, ≤ 63 → SUBHEADER,
     * ≤ 255 → BODY, > 255 → TAIL.
     * We pick the bucket matching the last byte's offset (largest region). */
    uint32_t bucket = netc_ctx_bucket((uint32_t)(src_size > 0 ? src_size - 1 : 0));
    const netc_tans_table_t *tbl = &dict->tables[bucket];

    if (!tbl->valid) return -1;

    /* Check that all symbols in src exist in the table */
    for (size_t i = 0; i < src_size; i++) {
        if (tbl->freq.freq[src[i]] == 0) return -1;
    }

    /* Need at least 4 bytes for initial_state */
    if (dst_payload_cap < 4) return -1;

    /* Bitstream writer over the region after the initial_state field */
    netc_bsw_t bsw;
    netc_bsw_init(&bsw, dst + 4, dst_payload_cap - 4);

    uint32_t final_state = netc_tans_encode(
        tbl, src, src_size, &bsw, NETC_TANS_TABLE_SIZE);

    if (final_state == 0) return -1;

    size_t bits_bytes = netc_bsw_flush(&bsw);
    if (bits_bytes == (size_t)-1) return -1;

    /* Write initial_state (the encoder's final state) LE */
    netc_write_u32_le(dst, final_state);
    *compressed_payload_size = 4 + bits_bytes;
    return 0;
}

/* =========================================================================
 * netc_compress — stateful context path
 * ========================================================================= */

netc_result_t netc_compress(
    netc_ctx_t *ctx,
    const void *src,
    size_t      src_size,
    void       *dst,
    size_t      dst_cap,
    size_t     *dst_size)
{
    if (NETC_UNLIKELY(ctx == NULL)) {
        return NETC_ERR_CTX_NULL;
    }
    if (NETC_UNLIKELY(src == NULL || dst == NULL || dst_size == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(src_size > NETC_MAX_PACKET_SIZE)) {
        return NETC_ERR_TOOBIG;
    }
    if (NETC_UNLIKELY(dst_cap < NETC_HEADER_SIZE)) {
        return NETC_ERR_BUF_SMALL;
    }

    uint8_t seq  = ctx->context_seq++;
    const netc_dict_t *dict = ctx->dict;

    /* Attempt tANS if we have a valid dictionary */
    if (dict != NULL && src_size > 0) {
        size_t payload_cap = dst_cap - NETC_HEADER_SIZE;
        uint8_t *payload   = (uint8_t *)dst + NETC_HEADER_SIZE;
        size_t  compressed_payload = 0;

        if (try_tans_compress(dict, (const uint8_t *)src, src_size,
                              payload, payload_cap, &compressed_payload) == 0) {
            /* Only use tANS if it actually compressed (AD-006) */
            if (compressed_payload < src_size) {
                netc_pkt_header_t hdr;
                hdr.original_size   = (uint16_t)src_size;
                hdr.compressed_size = (uint16_t)compressed_payload;
                hdr.flags           = NETC_PKT_FLAG_DICT_ID;
                hdr.algorithm       = NETC_ALG_TANS;
                hdr.model_id        = dict->model_id;
                hdr.context_seq     = seq;

                netc_hdr_write(dst, &hdr);
                *dst_size = NETC_HEADER_SIZE + compressed_payload;

                if (ctx->flags & NETC_CFG_FLAG_STATS) {
                    ctx->stats.packets_compressed++;
                    ctx->stats.bytes_in  += src_size;
                    ctx->stats.bytes_out += *dst_size;
                }
                return NETC_OK;
            }
        }
    }

    /* Fall back to passthrough */
    return emit_passthrough(ctx, dict, src, src_size, dst, dst_cap, dst_size, seq);
}

/* =========================================================================
 * netc_compress_stateless
 * ========================================================================= */

netc_result_t netc_compress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size)
{
    if (NETC_UNLIKELY(dict == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(src == NULL || dst == NULL || dst_size == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(src_size > NETC_MAX_PACKET_SIZE)) {
        return NETC_ERR_TOOBIG;
    }
    if (NETC_UNLIKELY(dst_cap < NETC_HEADER_SIZE)) {
        return NETC_ERR_BUF_SMALL;
    }

    if (src_size > 0) {
        size_t payload_cap = dst_cap - NETC_HEADER_SIZE;
        uint8_t *payload   = (uint8_t *)dst + NETC_HEADER_SIZE;
        size_t  compressed_payload = 0;

        if (try_tans_compress(dict, (const uint8_t *)src, src_size,
                              payload, payload_cap, &compressed_payload) == 0) {
            if (compressed_payload < src_size) {
                netc_pkt_header_t hdr;
                hdr.original_size   = (uint16_t)src_size;
                hdr.compressed_size = (uint16_t)compressed_payload;
                hdr.flags           = NETC_PKT_FLAG_DICT_ID;
                hdr.algorithm       = NETC_ALG_TANS;
                hdr.model_id        = dict->model_id;
                hdr.context_seq     = 0;

                netc_hdr_write(dst, &hdr);
                *dst_size = NETC_HEADER_SIZE + compressed_payload;
                return NETC_OK;
            }
        }
    }

    return emit_passthrough(NULL, dict, src, src_size, dst, dst_cap, dst_size, 0);
}
