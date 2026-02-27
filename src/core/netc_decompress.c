/**
 * netc_decompress.c — Decompression entry point.
 *
 * Phase 1: Passthrough baseline.
 *   - Reads and validates the 8-byte packet header.
 *   - Validates all security constraints (RFC-001 §15.1):
 *       - original_size ≤ NETC_MAX_PACKET_SIZE
 *       - original_size ≤ dst_cap
 *       - src_size ≥ NETC_HEADER_SIZE + compressed_size
 *   - If NETC_PKT_FLAG_PASSTHRU is set, copies payload verbatim.
 *   - All other algorithm codes return NETC_ERR_UNSUPPORTED in Phase 1.
 *
 * Phase 2: Will route to the tANS decoder for NETC_ALG_TANS packets.
 */

#include "netc_internal.h"
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

    /* Security: original_size must fit within limits and destination buffer */
    if (NETC_UNLIKELY(hdr_out->original_size > NETC_MAX_PACKET_SIZE)) {
        return NETC_ERR_CORRUPT;
    }
    if (NETC_UNLIKELY(hdr_out->original_size > dst_cap)) {
        return NETC_ERR_BUF_SMALL;
    }

    /* Security: src must contain at least header + compressed_payload */
    size_t expected = (size_t)NETC_HEADER_SIZE + (size_t)hdr_out->compressed_size;
    if (NETC_UNLIKELY(src_size < expected)) {
        return NETC_ERR_CORRUPT;
    }

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
            /* Passthrough: payload is the original bytes verbatim */
            if (NETC_UNLIKELY(hdr.compressed_size != hdr.original_size)) {
                return NETC_ERR_CORRUPT;
            }
            memcpy(dst, payload, hdr.original_size);
            *dst_size = hdr.original_size;

            /* Update statistics */
            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += hdr.original_size;
                ctx->stats.passthrough_count++;
            }

            /* Advance sequence counter */
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            return NETC_OK;
        }

        case NETC_ALG_TANS:
            /* Phase 2: tANS decode path */
            return NETC_ERR_UNSUPPORTED;

        case NETC_ALG_RANS:
            /* v0.2: rANS decode path */
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

    /* Validate model_id */
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
            return NETC_ERR_UNSUPPORTED;

        case NETC_ALG_RANS:
            return NETC_ERR_UNSUPPORTED;

        default:
            return NETC_ERR_CORRUPT;
    }
}
