/**
 * netc_compress.c — Compression entry point.
 *
 * Phase 1: Passthrough baseline.
 *   - Validates all arguments.
 *   - Emits the original bytes with NETC_PKT_FLAG_PASSTHRU | NETC_ALG_PASSTHRU.
 *   - Updates statistics if NETC_CFG_FLAG_STATS is set.
 *
 * Phase 2: Will insert the field-class delta pre-pass and tANS encode here,
 *   falling back to passthrough if compressed_size >= original_size (AD-006).
 */

#include "netc_internal.h"
#include <string.h>

/* =========================================================================
 * Internal: emit a passthrough packet
 * ========================================================================= */

static netc_result_t emit_passthrough(
    netc_ctx_t        *ctx,       /* may be NULL for stateless path */
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
    hdr.compressed_size = (uint16_t)src_size;  /* passthrough: same as original */
    hdr.flags           = NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DICT_ID;
    hdr.algorithm       = NETC_ALG_PASSTHRU;
    hdr.model_id        = (dict != NULL) ? dict->model_id : 0;
    hdr.context_seq     = context_seq;

    uint8_t *out = (uint8_t *)dst;
    netc_hdr_write(out, &hdr);
    memcpy(out + NETC_HEADER_SIZE, src, src_size);

    *dst_size = out_size;

    /* Update statistics */
    if (ctx != NULL && (ctx->flags & NETC_CFG_FLAG_STATS)) {
        ctx->stats.packets_compressed++;
        ctx->stats.bytes_in  += src_size;
        ctx->stats.bytes_out += out_size;
        ctx->stats.passthrough_count++;
    }

    return NETC_OK;
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
    /* --- Argument validation --- */
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

    /* Phase 1: always passthrough.
     * Phase 2: attempt tANS compression; fall back to passthrough on expansion. */
    uint8_t seq = ctx->context_seq++;

    netc_result_t r = emit_passthrough(
        ctx, ctx->dict, src, src_size, dst, dst_cap, dst_size, seq);

    return r;
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

    /* Stateless: context_seq = 0 (no persistent sequence state) */
    return emit_passthrough(NULL, dict, src, src_size, dst, dst_cap, dst_size, 0);
}
