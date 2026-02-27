/**
 * netc_decompress.c — Decompression entry point.
 *
 * Phase 2: Routes NETC_ALG_TANS packets to the tANS decoder.
 * Phase 3: Applies delta post-pass when NETC_PKT_FLAG_DELTA is set (AD-002).
 *
 *   - Reads and validates the 8-byte packet header.
 *   - Validates all security constraints (RFC-001 §15.1).
 *   - NETC_ALG_PASSTHRU: copies payload verbatim.
 *   - NETC_ALG_TANS: reads initial_state (4 bytes LE) then decodes bitstream.
 *   - If NETC_PKT_FLAG_DELTA is set: applies delta post-pass to reconstruct
 *     the original bytes from residuals + previous packet predictor.
 */

#include "netc_internal.h"
#include "../algo/netc_tans.h"
#include "../util/netc_bitstream.h"
#include <string.h>

/* =========================================================================
 * Internal: validate header and src buffer bounds
 * ========================================================================= */

static netc_result_t validate_header(
    const void         *src,
    size_t              src_size,
    size_t              dst_cap,
    netc_pkt_header_t  *hdr_out)
{
    if (NETC_UNLIKELY(src_size < NETC_HEADER_SIZE)) {
        return NETC_ERR_CORRUPT;
    }

    netc_hdr_read(src, hdr_out);

    if (NETC_UNLIKELY(hdr_out->original_size > NETC_MAX_PACKET_SIZE)) {
        return NETC_ERR_CORRUPT;
    }
    if (NETC_UNLIKELY(hdr_out->original_size > dst_cap)) {
        return NETC_ERR_BUF_SMALL;
    }

    size_t expected = (size_t)NETC_HEADER_SIZE + (size_t)hdr_out->compressed_size;
    if (NETC_UNLIKELY(src_size < expected)) {
        return NETC_ERR_CORRUPT;
    }

    return NETC_OK;
}

/* =========================================================================
 * Internal: tANS decode path
 *
 * Wire format after the 8-byte header:
 *   [4] initial_state (uint32 LE)
 *   [N] bitstream payload
 * ========================================================================= */

static netc_result_t decode_tans(
    const netc_dict_t     *dict,
    const netc_pkt_header_t *hdr,
    const uint8_t         *payload,     /* points past the 8-byte header */
    size_t                 payload_size, /* = hdr->compressed_size */
    void                  *dst,
    size_t                *dst_size)
{
    if (dict == NULL) return NETC_ERR_DICT_INVALID;
    if (payload_size < 4) return NETC_ERR_CORRUPT;

    uint32_t initial_state = netc_read_u32_le(payload);
    const uint8_t *bits    = payload + 4;
    size_t         bits_sz = payload_size - 4;

    /* Select the same bucket the encoder used */
    size_t orig = hdr->original_size;
    uint32_t bucket = netc_ctx_bucket((uint32_t)(orig > 0 ? orig - 1 : 0));
    const netc_tans_table_t *tbl = &dict->tables[bucket];

    if (!tbl->valid) return NETC_ERR_DICT_INVALID;

    /* Validate initial_state is in the expected range */
    if (initial_state < NETC_TANS_TABLE_SIZE ||
        initial_state >= 2U * NETC_TANS_TABLE_SIZE) {
        return NETC_ERR_CORRUPT;
    }

    netc_bsr_t bsr;
    netc_bsr_init(&bsr, bits, bits_sz);

    if (netc_tans_decode(tbl, &bsr, (uint8_t *)dst, orig, initial_state) != 0) {
        return NETC_ERR_CORRUPT;
    }

    *dst_size = orig;
    return NETC_OK;
}

/* =========================================================================
 * netc_decompress — stateful context path
 * ========================================================================= */

netc_result_t netc_decompress(
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

    netc_pkt_header_t hdr;
    netc_result_t r = validate_header(src, src_size, dst_cap, &hdr);
    if (NETC_UNLIKELY(r != NETC_OK)) {
        return r;
    }

    /* Validate model_id if a dictionary is loaded */
    if (ctx->dict != NULL && !(hdr.flags & NETC_PKT_FLAG_PASSTHRU)) {
        if (NETC_UNLIKELY(hdr.model_id != ctx->dict->model_id)) {
            return NETC_ERR_VERSION;
        }
    }

    const uint8_t *payload = (const uint8_t *)src + NETC_HEADER_SIZE;

    switch (hdr.algorithm) {
        case NETC_ALG_PASSTHRU: {
            if (NETC_UNLIKELY(hdr.compressed_size != hdr.original_size)) {
                return NETC_ERR_CORRUPT;
            }
            memcpy(dst, payload, hdr.original_size);
            *dst_size = hdr.original_size;

            /* Update delta predictor with this packet's original bytes */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, hdr.original_size);
                ctx->prev_pkt_size = hdr.original_size;
            }

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += hdr.original_size;
                ctx->stats.passthrough_count++;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            return NETC_OK;
        }

        case NETC_ALG_TANS: {
            r = decode_tans(ctx->dict, &hdr, payload,
                            hdr.compressed_size, dst, dst_size);
            if (r != NETC_OK) return r;

            /* Phase 3: Delta post-pass — undo delta encoding if flag is set */
            if ((hdr.flags & NETC_PKT_FLAG_DELTA) &&
                ctx->prev_pkt != NULL &&
                ctx->prev_pkt_size == *dst_size)
            {
                /* dst currently holds residuals; reconstruct original in-place
                 * via SIMD dispatch */
                ctx->simd_ops.delta_decode(ctx->prev_pkt, (const uint8_t *)dst,
                                           (uint8_t *)dst, *dst_size);
            }

            /* Update delta predictor with reconstructed original bytes */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, *dst_size);
                ctx->prev_pkt_size = *dst_size;
            }

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            return NETC_OK;
        }

        case NETC_ALG_RANS:
            return NETC_ERR_UNSUPPORTED;

        default:
            return NETC_ERR_CORRUPT;
    }
}

/* =========================================================================
 * netc_decompress_stateless
 * ========================================================================= */

netc_result_t netc_decompress_stateless(
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

    netc_pkt_header_t hdr;
    netc_result_t r = validate_header(src, src_size, dst_cap, &hdr);
    if (NETC_UNLIKELY(r != NETC_OK)) {
        return r;
    }

    if (!(hdr.flags & NETC_PKT_FLAG_PASSTHRU)) {
        if (NETC_UNLIKELY(hdr.model_id != dict->model_id)) {
            return NETC_ERR_VERSION;
        }
    }

    const uint8_t *payload = (const uint8_t *)src + NETC_HEADER_SIZE;

    switch (hdr.algorithm) {
        case NETC_ALG_PASSTHRU: {
            if (NETC_UNLIKELY(hdr.compressed_size != hdr.original_size)) {
                return NETC_ERR_CORRUPT;
            }
            memcpy(dst, payload, hdr.original_size);
            *dst_size = hdr.original_size;
            return NETC_OK;
        }

        case NETC_ALG_TANS:
            return decode_tans(dict, &hdr, payload,
                               hdr.compressed_size, dst, dst_size);

        case NETC_ALG_RANS:
            return NETC_ERR_UNSUPPORTED;

        default:
            return NETC_ERR_CORRUPT;
    }
}
