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
 * Internal: LZ77 decode
 *
 * Inverse of lz77_encode in netc_compress.c.
 * Token format:
 *   [0lllllll]              → literal run: read bits[6:0]+1 raw bytes
 *   [1lllllll][oooooooo]    → back-ref: match_len=bits[6:0]+3, offset=byte+1
 *
 * Returns NETC_OK on success, NETC_ERR_CORRUPT on malformed input.
 * ========================================================================= */
static netc_result_t lz77_decode(const uint8_t *lz_src, size_t lz_size,
                                  uint8_t *dst, size_t orig_size)
{
    size_t out = 0;
    size_t i   = 0;
    while (i < lz_size) {
        uint8_t tok = lz_src[i++];
        if (tok & 0x80u) {
            /* Back-reference */
            if (i >= lz_size) return NETC_ERR_CORRUPT;
            size_t match_len = (size_t)(tok & 0x7Fu) + 3;
            size_t offset    = (size_t)lz_src[i++] + 1;
            if (offset > out) return NETC_ERR_CORRUPT;
            if (out + match_len > orig_size) return NETC_ERR_CORRUPT;
            size_t copy_from = out - offset;
            /* Byte-by-byte copy to handle overlapping (RLE-style) runs */
            for (size_t k = 0; k < match_len; k++)
                dst[out + k] = dst[copy_from + k];
            out += match_len;
        } else {
            /* Literal run */
            size_t lit_len = (size_t)(tok & 0x7Fu) + 1;
            if (i + lit_len > lz_size) return NETC_ERR_CORRUPT;
            if (out + lit_len > orig_size) return NETC_ERR_CORRUPT;
            memcpy(dst + out, lz_src + i, lit_len);
            out += lit_len;
            i   += lit_len;
        }
    }
    if (out != orig_size) return NETC_ERR_CORRUPT;
    return NETC_OK;
}

/* =========================================================================
 * Internal: RLE decode
 *
 * Inverse of rle_encode in netc_compress.c.
 * rle_src: (count, symbol) pairs; rle_size: byte count of the RLE stream.
 * dst: output buffer; orig_size: expected decompressed byte count.
 * Returns NETC_OK on success or NETC_ERR_CORRUPT on malformed input.
 * ========================================================================= */
static netc_result_t rle_decode(const uint8_t *rle_src, size_t rle_size,
                                 uint8_t *dst, size_t orig_size)
{
    size_t out = 0;
    size_t i   = 0;
    while (i + 1 < rle_size) {
        uint8_t count = rle_src[i];
        uint8_t sym   = rle_src[i + 1];
        i += 2;
        if (count == 0 || out + count > orig_size) return NETC_ERR_CORRUPT;
        memset(dst + out, sym, count);
        out += count;
    }
    if (i != rle_size || out != orig_size) return NETC_ERR_CORRUPT;
    return NETC_OK;
}

/* =========================================================================
 * Internal: bucket offset boundaries (mirrors netc_compress.c)
 * ========================================================================= */
static uint32_t decomp_bucket_start(uint32_t b) {
    static const uint32_t starts[NETC_CTX_COUNT] = {
           0,    8,   16,   24,   32,   48,   64,   96,
         128,  192,  256,  384,  512, 1024, 4096, 16384
    };
    return (b < NETC_CTX_COUNT) ? starts[b] : 65536U;
}

/* =========================================================================
 * Internal: tANS decode path (v0.2: multi-region + RLE support)
 *
 * MREG wire format after the 8-byte packet header:
 *   [1B]      n_regions
 *   [n×8B]    per-region {uint32_le initial_state, uint32_le bitstream_bytes}
 *   [N B]     concatenated bitstreams (region 0 first)
 *
 * If NETC_PKT_FLAG_RLE is set, the tANS-decoded output is the RLE stream;
 * a subsequent rle_decode pass reconstructs the original bytes.
 * ========================================================================= */

static netc_result_t decode_tans(
    const netc_dict_t       *dict,
    const netc_pkt_header_t *hdr,
    const uint8_t           *payload,     /* points past the 8-byte header */
    size_t                   payload_size, /* = hdr->compressed_size */
    void                    *dst,
    size_t                  *dst_size,
    uint8_t                 *scratch,      /* unused, kept for future use */
    size_t                   scratch_cap)  /* unused, kept for future use */
{
    (void)scratch; (void)scratch_cap;
    if (dict == NULL) return NETC_ERR_DICT_INVALID;

    size_t orig = hdr->original_size;
    int is_mreg = (hdr->flags & NETC_PKT_FLAG_MREG) != 0;

    if (is_mreg) {
        /* --- Multi-region decode (v0.2) --- */
        if (payload_size < 1) return NETC_ERR_CORRUPT;
        uint8_t n_regions = payload[0];
        if (n_regions == 0 || n_regions > NETC_CTX_COUNT) return NETC_ERR_CORRUPT;

        size_t hdr_bytes = 1u + (size_t)n_regions * 8u;
        if (payload_size < hdr_bytes) return NETC_ERR_CORRUPT;

        const uint8_t *bits_base  = payload + hdr_bytes;
        size_t         bits_avail = payload_size - hdr_bytes;
        size_t         bits_offset = 0;

        uint32_t first_bucket = netc_ctx_bucket(0);

        for (uint32_t r = 0; r < n_regions; r++) {
            uint32_t state    = netc_read_u32_le(payload + 1u + r * 8u);
            uint32_t bs_bytes = netc_read_u32_le(payload + 1u + r * 8u + 4u);

            if (state == 0 && bs_bytes == 0) continue; /* empty region sentinel */

            uint32_t bucket     = first_bucket + r;
            uint32_t rstart     = decomp_bucket_start(bucket);
            uint32_t rend_bound = decomp_bucket_start(bucket + 1);

            size_t region_start = (rstart     < orig) ? (size_t)rstart     : orig;
            size_t region_end   = (rend_bound < orig) ? (size_t)rend_bound : orig;
            size_t region_len   = region_end - region_start;

            if (region_len == 0) continue;

            if (state < NETC_TANS_TABLE_SIZE || state >= 2U * NETC_TANS_TABLE_SIZE)
                return NETC_ERR_CORRUPT;
            if (bits_offset + bs_bytes > bits_avail)
                return NETC_ERR_CORRUPT;

            const netc_tans_table_t *tbl = &dict->tables[bucket];
            if (!tbl->valid) return NETC_ERR_DICT_INVALID;

            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits_base + bits_offset, bs_bytes);

            if (netc_tans_decode(tbl, &bsr, (uint8_t *)dst + region_start,
                                 region_len, state) != 0)
                return NETC_ERR_CORRUPT;

            bits_offset += bs_bytes;
        }

        *dst_size = orig;
        return NETC_OK;

    } else {
        /* --- Legacy single-region decode (no MREG flag) --- */
        uint32_t bucket = netc_ctx_bucket(0);  /* single region always bucket 0 */
        const netc_tans_table_t *tbl = &dict->tables[bucket];
        if (!tbl->valid) return NETC_ERR_DICT_INVALID;

        if (hdr->flags & NETC_PKT_FLAG_X2) {
            /* Dual-interleaved x2: [4B state0][4B state1][bitstream] */
            if (payload_size < 8) return NETC_ERR_CORRUPT;
            uint32_t state0 = netc_read_u32_le(payload);
            uint32_t state1 = netc_read_u32_le(payload + 4);
            const uint8_t *bits = payload + 8;
            size_t         bits_sz = payload_size - 8;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);
            if (netc_tans_decode_x2(tbl, &bsr, (uint8_t *)dst, orig,
                                    state0, state1) != 0)
                return NETC_ERR_CORRUPT;
        } else {
            /* Single-state: [4B state][bitstream] */
            if (payload_size < 4) return NETC_ERR_CORRUPT;
            uint32_t initial_state = netc_read_u32_le(payload);
            const uint8_t *bits    = payload + 4;
            size_t         bits_sz = payload_size - 4;
            if (initial_state < NETC_TANS_TABLE_SIZE ||
                initial_state >= 2U * NETC_TANS_TABLE_SIZE)
                return NETC_ERR_CORRUPT;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);
            if (netc_tans_decode(tbl, &bsr, (uint8_t *)dst, orig,
                                 initial_state) != 0)
                return NETC_ERR_CORRUPT;
        }

        *dst_size = orig;
        return NETC_OK;
    }
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
            if (hdr.flags & NETC_PKT_FLAG_LZ77) {
                /* LZ77 passthrough: payload is an LZ77 stream */
                netc_result_t rr = lz77_decode(payload, hdr.compressed_size,
                                               (uint8_t *)dst, hdr.original_size);
                if (rr != NETC_OK) return rr;
            } else if (hdr.flags & NETC_PKT_FLAG_RLE) {
                /* RLE-only passthrough: payload is RLE-encoded, decode into dst */
                netc_result_t rr = rle_decode(payload, hdr.compressed_size,
                                              (uint8_t *)dst, hdr.original_size);
                if (rr != NETC_OK) return rr;
            } else {
                if (NETC_UNLIKELY(hdr.compressed_size != hdr.original_size)) {
                    return NETC_ERR_CORRUPT;
                }
                memcpy(dst, payload, hdr.original_size);
            }
            *dst_size = hdr.original_size;

            /* Delta post-pass for LZ77+DELTA: dst holds residuals from LZ77
             * decoding; reconstruct original using the prev_pkt predictor. */
            if ((hdr.flags & (NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_LZ77)) ==
                (NETC_PKT_FLAG_DELTA | NETC_PKT_FLAG_LZ77) &&
                ctx->prev_pkt != NULL &&
                ctx->prev_pkt_size == hdr.original_size)
            {
                ctx->simd_ops.delta_decode(ctx->prev_pkt, (const uint8_t *)dst,
                                           (uint8_t *)dst, hdr.original_size);
            }

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
            uint8_t *scratch    = ctx->arena;
            size_t   scratch_cap = ctx->arena_size;
            r = decode_tans(ctx->dict, &hdr, payload,
                            hdr.compressed_size, dst, dst_size,
                            scratch, scratch_cap);
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
            if (hdr.flags & NETC_PKT_FLAG_LZ77) {
                /* LZ77 passthrough — decode directly into dst */
                netc_result_t rr = lz77_decode(payload, hdr.compressed_size,
                                               (uint8_t *)dst, hdr.original_size);
                if (rr != NETC_OK) return rr;
            } else if (hdr.flags & NETC_PKT_FLAG_RLE) {
                /* RLE-only passthrough — stateless path has no arena;
                 * decode directly into dst (no intermediate buffer needed) */
                netc_result_t rr = rle_decode(payload, hdr.compressed_size,
                                              (uint8_t *)dst, hdr.original_size);
                if (rr != NETC_OK) return rr;
            } else {
                if (NETC_UNLIKELY(hdr.compressed_size != hdr.original_size)) {
                    return NETC_ERR_CORRUPT;
                }
                memcpy(dst, payload, hdr.original_size);
            }
            *dst_size = hdr.original_size;
            return NETC_OK;
        }

        case NETC_ALG_TANS:
            return decode_tans(dict, &hdr, payload,
                               hdr.compressed_size, dst, dst_size,
                               NULL, 0);

        case NETC_ALG_RANS:
            return NETC_ERR_UNSUPPORTED;

        default:
            return NETC_ERR_CORRUPT;
    }
}
