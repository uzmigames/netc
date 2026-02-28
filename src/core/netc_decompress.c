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
#include "../algo/netc_adaptive.h"
#include "../util/netc_bitstream.h"
#include <string.h>

/* =========================================================================
 * Internal: validate header and src buffer bounds
 * ========================================================================= */

static netc_result_t validate_header(
    const void         *src,
    size_t              src_size,
    size_t              dst_cap,
    netc_pkt_header_t  *hdr_out,
    int                 compact,
    size_t             *hdr_size_out)
{
    size_t hdr_sz;

    if (compact) {
        hdr_sz = netc_hdr_read_compact(src, src_size, hdr_out);
        if (NETC_UNLIKELY(hdr_sz == 0u)) return NETC_ERR_CORRUPT;
        /* compressed_size is derived from transport packet length */
        hdr_out->compressed_size = (uint16_t)(src_size - hdr_sz);
    } else {
        if (NETC_UNLIKELY(src_size < NETC_HEADER_SIZE)) return NETC_ERR_CORRUPT;
        netc_hdr_read(src, hdr_out);
        hdr_sz = NETC_HEADER_SIZE;
    }

    if (NETC_UNLIKELY(hdr_out->original_size > NETC_MAX_PACKET_SIZE)) {
        return NETC_ERR_CORRUPT;
    }
    if (NETC_UNLIKELY(hdr_out->original_size > dst_cap)) {
        return NETC_ERR_BUF_SMALL;
    }

    /* Validate total packet length (legacy only — compact derives compressed_size) */
    if (!compact) {
        size_t expected = hdr_sz + (size_t)hdr_out->compressed_size;
        if (NETC_UNLIKELY(src_size < expected)) return NETC_ERR_CORRUPT;
    }

    *hdr_size_out = hdr_sz;
    return NETC_OK;
}

/* =========================================================================
 * Internal: ring buffer append (mirrors ctx_ring_append in netc_compress.c)
 * ========================================================================= */

static void decomp_ring_append(netc_ctx_t *ctx,
                                const uint8_t *data, size_t len)
{
    if (ctx->ring == NULL || ctx->ring_size == 0 || len == 0) return;
    uint32_t rs  = ctx->ring_size;
    uint32_t pos = ctx->ring_pos;
    if (len >= rs) {
        data += len - rs;
        len   = rs;
        pos   = 0;
    }
    size_t tail = rs - pos;
    if (len <= tail) {
        memcpy(ctx->ring + pos, data, len);
    } else {
        memcpy(ctx->ring + pos, data, tail);
        memcpy(ctx->ring,       data + tail, len - tail);
    }
    ctx->ring_pos = (uint32_t)((pos + len) % rs);
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
 * Internal: cross-packet LZ77 decode (NETC_ALG_LZ77X)
 *
 * Inverse of lz77x_encode in netc_compress.c.
 * Token format:
 *   [0lllllll]                     literal run: len=bits[6:0]+1 (1–128)
 *   [10llllll][oooooooo]            short back-ref: len=bits[5:0]+3, offset=byte+1 (1–256)
 *                                   counts back from current OUTPUT position only
 *   [11llllll][lo][hi]              long back-ref: len=bits[5:0]+3, offset=u16le+1 (1–65536)
 *                                   counts back into ring+dst virtual buffer
 *
 * For long back-refs, the virtual buffer is:
 *   [...ring history...][...dst decoded so far...]
 * Offset 1 = dst[out-1] (most recent output byte),
 * Offset > out references into ring buffer.
 *
 * Returns NETC_OK on success, NETC_ERR_CORRUPT on malformed input.
 * ========================================================================= */
static netc_result_t lz77x_decode(
    const uint8_t *lz_src,   size_t lz_size,
    uint8_t       *dst,       size_t orig_size,
    const uint8_t *ring,      uint32_t ring_size, uint32_t ring_pos)
{
    size_t out = 0;
    size_t i   = 0;

    while (i < lz_size) {
        uint8_t tok = lz_src[i++];

        if (!(tok & 0x80u)) {
            /* Literal run: [0lllllll] → len = bits[6:0]+1 */
            size_t lit_len = (size_t)(tok & 0x7Fu) + 1;
            if (i + lit_len > lz_size)      return NETC_ERR_CORRUPT;
            if (out + lit_len > orig_size)  return NETC_ERR_CORRUPT;
            memcpy(dst + out, lz_src + i, lit_len);
            out += lit_len;
            i   += lit_len;

        } else if (!(tok & 0x40u)) {
            /* Short back-ref: [10llllll][oooooooo] */
            if (i >= lz_size)               return NETC_ERR_CORRUPT;
            size_t match_len = (size_t)(tok & 0x3Fu) + 3;
            size_t offset    = (size_t)lz_src[i++] + 1;
            if (offset > out)               return NETC_ERR_CORRUPT;
            if (out + match_len > orig_size) return NETC_ERR_CORRUPT;
            size_t copy_from = out - offset;
            for (size_t k = 0; k < match_len; k++)
                dst[out + k] = dst[copy_from + k];
            out += match_len;

        } else {
            /* Long back-ref: [11llllll][lo][hi]
             * offset = ring distance from ring_pos back to match start.
             * Match: ring[(ring_pos - offset + k) % ring_size] for k=0..match_len-1. */
            if (i + 2 > lz_size)            return NETC_ERR_CORRUPT;
            size_t match_len = (size_t)(tok & 0x3Fu) + 3;
            uint16_t off16   = (uint16_t)lz_src[i] | ((uint16_t)lz_src[i + 1] << 8);
            i += 2;
            size_t offset = (size_t)off16 + 1;  /* 1-based ring distance */

            if (ring == NULL || ring_size == 0)  return NETC_ERR_CORRUPT;
            if (offset > ring_size)              return NETC_ERR_CORRUPT;
            if (out + match_len > orig_size)     return NETC_ERR_CORRUPT;

            uint32_t rstart = (ring_pos + ring_size - (uint32_t)offset) % ring_size;
            for (size_t k = 0; k < match_len; k++) {
                dst[out++] = ring[(rstart + k) % ring_size];
            }
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

/* Helper: select tANS table for decode -- mirrors select_tans_table in netc_compress.c. */
static const netc_tans_table_t *
decomp_select_tbl(const netc_dict_t *dict,
                  const netc_tans_table_t *tables,
                  uint32_t bucket,
                  uint8_t prev_byte, uint8_t pkt_flags)
{
    if (pkt_flags & NETC_PKT_FLAG_BIGRAM) {
        uint32_t bclass = netc_bigram_class(prev_byte, dict->bigram_class_map);
        const netc_tans_table_t *tbl = &dict->bigram_tables[bucket][bclass];
        if (tbl->valid) return tbl;
    }
    return &tables[bucket];
}

static netc_result_t decode_tans(
    const netc_dict_t       *dict,
    const netc_tans_table_t *tables,       /* adaptive or dict->tables */
    const netc_pkt_header_t *hdr,
    const uint8_t           *payload,     /* points past the packet header */
    size_t                   payload_size, /* = hdr->compressed_size */
    void                    *dst,
    size_t                  *dst_size,
    uint8_t                 *scratch,      /* unused, kept for future use */
    size_t                   scratch_cap,  /* unused, kept for future use */
    int                      compact)      /* compact mode: 2B ANS state */
{
    (void)scratch; (void)scratch_cap;
    if (dict == NULL) return NETC_ERR_DICT_INVALID;

    size_t orig = hdr->original_size;
    int is_mreg = (hdr->flags & NETC_PKT_FLAG_MREG) != 0;

    /* ANS state is 2B in compact mode, 4B in legacy mode */
    const size_t state1_sz = compact ? 2u : 4u;
    const size_t state2_sz = compact ? 4u : 8u;

    if (is_mreg) {
        /* --- Multi-region decode (v0.2+) ---
         * MREG is never produced in compact mode (encoder always uses PCTX),
         * but we keep legacy 4B state reads for backward compat. */
        if (payload_size < 1) return NETC_ERR_CORRUPT;
        uint8_t n_regions = payload[0];
        if (n_regions == 0 || n_regions > NETC_CTX_COUNT) return NETC_ERR_CORRUPT;

        size_t hdr_bytes = 1u + (size_t)n_regions * 8u;
        if (payload_size < hdr_bytes) return NETC_ERR_CORRUPT;

        const uint8_t *bits_base  = payload + hdr_bytes;
        size_t         bits_avail = payload_size - hdr_bytes;
        size_t         bits_offset = 0;

        uint32_t first_bucket = netc_ctx_bucket(0);

        /* prev_byte tracks the last decoded byte of the previous region,
         * matching what the encoder used for bigram class selection. */
        uint8_t region_prev_byte = 0x00u;

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

            const netc_tans_table_t *tbl = decomp_select_tbl(dict, tables, bucket,
                                                              region_prev_byte,
                                                              hdr->flags);
            if (!tbl->valid) return NETC_ERR_DICT_INVALID;

            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits_base + bits_offset, bs_bytes);

            if (netc_tans_decode(tbl, &bsr, (uint8_t *)dst + region_start,
                                 region_len, state) != 0)
                return NETC_ERR_CORRUPT;

            bits_offset += bs_bytes;
            /* Update prev_byte for next region: last decoded byte of this region */
            region_prev_byte = ((uint8_t *)dst)[region_end - 1];
        }

        *dst_size = orig;
        return NETC_OK;

    } else {
        /* --- Single-region decode (no MREG flag) ---
         * The table bucket index is encoded in the upper 4 bits of hdr->algorithm.
         * Legacy packets have algorithm=0x01 → upper bits=0 → bucket 0 (backward-compat).
         * When the encoder picks a best-fit table for small multi-bucket packets,
         * it sets algorithm = NETC_ALG_TANS | (tbl_idx << 4). */
        uint32_t bucket = (uint32_t)(hdr->algorithm >> 4);
        if (bucket >= NETC_CTX_COUNT) bucket = 0; /* safety clamp */
        /* prev_byte at position 0 is implicitly 0x00 (packet start), same as encoder */
        const netc_tans_table_t *tbl = decomp_select_tbl(dict, tables, bucket, 0x00u, hdr->flags);
        if (!tbl->valid) return NETC_ERR_DICT_INVALID;

        if (hdr->flags & NETC_PKT_FLAG_X2) {
            /* Dual-interleaved x2: [state0][state1][bitstream] */
            if (payload_size < state2_sz) return NETC_ERR_CORRUPT;
            uint32_t state0, state1;
            if (compact) {
                state0 = netc_read_u16_le(payload);
                state1 = netc_read_u16_le(payload + 2);
            } else {
                state0 = netc_read_u32_le(payload);
                state1 = netc_read_u32_le(payload + 4);
            }
            const uint8_t *bits = payload + state2_sz;
            size_t         bits_sz = payload_size - state2_sz;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);
            if (netc_tans_decode_x2(tbl, &bsr, (uint8_t *)dst, orig,
                                    state0, state1) != 0)
                return NETC_ERR_CORRUPT;
        } else {
            /* Single-state: [state][bitstream] */
            if (payload_size < state1_sz) return NETC_ERR_CORRUPT;
            uint32_t initial_state;
            if (compact)
                initial_state = netc_read_u16_le(payload);
            else
                initial_state = netc_read_u32_le(payload);
            const uint8_t *bits    = payload + state1_sz;
            size_t         bits_sz = payload_size - state1_sz;
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

    const int compact_mode = (ctx->flags & NETC_CFG_FLAG_COMPACT_HDR) ? 1 : 0;
    /* Adaptive tables (when active) or frozen dict tables */
    const netc_tans_table_t *tables = (ctx->dict != NULL) ? netc_get_tables(ctx) : NULL;

    netc_pkt_header_t hdr;
    size_t pkt_hdr_sz = 0;
    netc_result_t r = validate_header(src, src_size, dst_cap, &hdr,
                                       compact_mode, &pkt_hdr_sz);
    if (NETC_UNLIKELY(r != NETC_OK)) {
        return r;
    }

    /* In compact mode, model_id/context_seq are not on the wire — fill from ctx */
    if (compact_mode) {
        hdr.model_id    = (ctx->dict != NULL) ? ctx->dict->model_id : 0;
        hdr.context_seq = ctx->context_seq;
    }

    /* Validate model_id if a dictionary is loaded and the packet uses entropy
     * coding (i.e. not a pure passthrough packet, not LZ77X). */
    if (ctx->dict != NULL &&
        !(hdr.flags & NETC_PKT_FLAG_PASSTHRU) &&
        hdr.algorithm != NETC_ALG_LZ77X) {
        if (NETC_UNLIKELY(hdr.model_id != ctx->dict->model_id)) {
            return NETC_ERR_VERSION;
        }
    }

    const uint8_t *payload = (const uint8_t *)src + pkt_hdr_sz;
    /* Upper 4 bits of the algorithm byte encode the table bucket index for
     * single-region tANS/LZP packets (set by encoder when using best-fit
     * table selection for small multi-bucket packets).  Normalize when the
     * low 4 bits are NETC_ALG_TANS, NETC_ALG_LZP, or NETC_ALG_TANS_10. */
    uint8_t alg_id = hdr.algorithm;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS)      alg_id = NETC_ALG_TANS;
    if ((alg_id & 0x0Fu) == NETC_ALG_LZP)       alg_id = NETC_ALG_LZP;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS_PCTX) alg_id = NETC_ALG_TANS_PCTX;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS_10)   alg_id = NETC_ALG_TANS_10;

    switch (alg_id) {
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
            /* Ring buffer update — keeps encoder/decoder history in sync */
            decomp_ring_append(ctx, (const uint8_t *)dst, hdr.original_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += hdr.original_size;
                ctx->stats.passthrough_count++;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, hdr.original_size);
            return NETC_OK;
        }

        case NETC_ALG_TANS: {
            uint8_t *scratch    = ctx->arena;
            size_t   scratch_cap = ctx->arena_size;
            r = decode_tans(ctx->dict, tables, &hdr, payload,
                            hdr.compressed_size, dst, dst_size,
                            scratch, scratch_cap, compact_mode);
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
            /* Ring buffer update */
            decomp_ring_append(ctx, (const uint8_t *)dst, *dst_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, *dst_size);
            return NETC_OK;
        }

        case NETC_ALG_TANS_PCTX: {
            /* Per-position context-adaptive tANS: single stream, table switches
             * per byte offset.  Wire format: [state_sz initial_state][bitstream].
             * When BIGRAM flag is set, also switches bigram class per byte. */
            if (ctx->dict == NULL) return NETC_ERR_DICT_INVALID;
            const size_t pctx_state_sz = compact_mode ? 2u : 4u;
            if (hdr.compressed_size < pctx_state_sz) return NETC_ERR_CORRUPT;

            uint32_t initial_state = compact_mode
                ? (uint32_t)netc_read_u16_le(payload)
                : netc_read_u32_le(payload);
            if (initial_state < NETC_TANS_TABLE_SIZE ||
                initial_state >= 2U * NETC_TANS_TABLE_SIZE)
                return NETC_ERR_CORRUPT;

            const uint8_t *bits = payload + pctx_state_sz;
            size_t bits_sz = hdr.compressed_size - pctx_state_sz;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);

            int pctx_rc;
            if ((hdr.flags & NETC_PKT_FLAG_BIGRAM) &&
                ctx->dict->bigram_tables[0][0].valid) {
                pctx_rc = netc_tans_decode_pctx_bigram(
                    ctx->dict->bigram_tables, tables,
                    ctx->dict->bigram_class_map,
                    &bsr, (uint8_t *)dst, hdr.original_size, initial_state);
            } else {
                pctx_rc = netc_tans_decode_pctx(
                    tables, &bsr,
                    (uint8_t *)dst, hdr.original_size, initial_state);
            }
            if (pctx_rc != 0)
                return NETC_ERR_CORRUPT;

            *dst_size = hdr.original_size;

            /* LZP XOR inverse: upper nibble of algorithm byte signals LZP
             * was applied as a pre-filter before PCTX encoding. */
            if ((hdr.algorithm & 0xF0u) != 0 &&
                ctx->dict->lzp_table != NULL)
            {
                netc_lzp_xor_unfilter((const uint8_t *)dst, *dst_size,
                                       ctx->dict->lzp_table, (uint8_t *)dst);
            }

            /* Delta post-pass */
            if ((hdr.flags & NETC_PKT_FLAG_DELTA) &&
                ctx->prev_pkt != NULL &&
                ctx->prev_pkt_size == *dst_size)
            {
                ctx->simd_ops.delta_decode(ctx->prev_pkt, (const uint8_t *)dst,
                                           (uint8_t *)dst, *dst_size);
            }

            /* Update delta predictor */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, *dst_size);
                ctx->prev_pkt_size = *dst_size;
            }
            /* Ring buffer update */
            decomp_ring_append(ctx, (const uint8_t *)dst, *dst_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, *dst_size);
            return NETC_OK;
        }

        case NETC_ALG_LZ77X: {
            /* Cross-packet LZ77: decode using ring buffer as history.
             * No delta flag — always encodes original (raw) src bytes. */
            if (ctx->ring == NULL || ctx->ring_size == 0) return NETC_ERR_UNSUPPORTED;
            r = lz77x_decode(payload, hdr.compressed_size,
                             (uint8_t *)dst, hdr.original_size,
                             ctx->ring, ctx->ring_size, ctx->ring_pos);
            if (r != NETC_OK) return r;
            *dst_size = hdr.original_size;

            /* Update delta predictor */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, *dst_size);
                ctx->prev_pkt_size = *dst_size;
            }
            /* Ring buffer update — MUST happen after decode (ring used as input above) */
            decomp_ring_append(ctx, (const uint8_t *)dst, *dst_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, *dst_size);
            return NETC_OK;
        }

        case NETC_ALG_LZP: {
            /* LZP XOR + tANS: wire format is identical to NETC_ALG_TANS
             * (same MREG/X2/BIGRAM sub-flags), but after tANS decode we
             * apply the LZP XOR inverse filter to recover original bytes. */
            if (ctx->dict == NULL || ctx->dict->lzp_table == NULL)
                return NETC_ERR_DICT_INVALID;

            r = decode_tans(ctx->dict, tables, &hdr, payload,
                            hdr.compressed_size, dst, dst_size,
                            ctx->arena, ctx->arena_size, compact_mode);
            if (r != NETC_OK) return r;

            /* LZP XOR inverse: undo the XOR pre-filter applied during
             * compression.  Operates in-place since netc_lzp_xor_unfilter
             * reads from src and writes to dst (can alias for in-place). */
            netc_lzp_xor_unfilter((const uint8_t *)dst, *dst_size,
                                   ctx->dict->lzp_table, (uint8_t *)dst);

            /* Delta post-pass (if delta was also applied) */
            if ((hdr.flags & NETC_PKT_FLAG_DELTA) &&
                ctx->prev_pkt != NULL &&
                ctx->prev_pkt_size == *dst_size)
            {
                ctx->simd_ops.delta_decode(ctx->prev_pkt, (const uint8_t *)dst,
                                           (uint8_t *)dst, *dst_size);
            }

            /* Update delta predictor */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, *dst_size);
                ctx->prev_pkt_size = *dst_size;
            }
            /* Ring buffer update */
            decomp_ring_append(ctx, (const uint8_t *)dst, *dst_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, *dst_size);
            return NETC_OK;
        }

        case NETC_ALG_TANS_10: {
            /* 10-bit tANS: small-packet optimization.
             * Wire format: [2B state (uint16 LE)][bitstream].
             * State range: [1024, 2048).
             * The table bucket index is encoded in hdr->algorithm upper nibble.
             * We rescale the 12-bit freq table to 10-bit and build on the fly. */
            if (ctx->dict == NULL) return NETC_ERR_DICT_INVALID;
            uint32_t bucket = (uint32_t)(hdr.algorithm >> 4);
            if (bucket >= NETC_CTX_COUNT) bucket = 0;
            const netc_tans_table_t *tbl12 = &tables[bucket];
            if (!tbl12->valid) return NETC_ERR_DICT_INVALID;

            /* 10-bit state is always 2 bytes */
            if (hdr.compressed_size < 2) return NETC_ERR_CORRUPT;
            uint32_t initial_state = (uint32_t)netc_read_u16_le(payload);
            if (initial_state < NETC_TANS_TABLE_SIZE_10 ||
                initial_state >= 2U * NETC_TANS_TABLE_SIZE_10)
                return NETC_ERR_CORRUPT;

            /* Rescale 12-bit freq table to 10-bit and build decode table */
            netc_freq_table_t freq10;
            if (netc_freq_rescale_12_to_10(&tbl12->freq, &freq10) != 0)
                return NETC_ERR_DICT_INVALID;
            netc_tans_table_10_t tbl10;
            if (netc_tans_build_10(&tbl10, &freq10) != 0)
                return NETC_ERR_DICT_INVALID;

            const uint8_t *bits = payload + 2;
            size_t bits_sz = hdr.compressed_size - 2;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);
            if (netc_tans_decode_10(&tbl10, &bsr, (uint8_t *)dst,
                                     hdr.original_size, initial_state) != 0)
                return NETC_ERR_CORRUPT;

            *dst_size = hdr.original_size;

            /* Delta post-pass */
            if ((hdr.flags & NETC_PKT_FLAG_DELTA) &&
                ctx->prev_pkt != NULL &&
                ctx->prev_pkt_size == *dst_size)
            {
                ctx->simd_ops.delta_decode(ctx->prev_pkt, (const uint8_t *)dst,
                                           (uint8_t *)dst, *dst_size);
            }

            /* Update delta predictor */
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, dst, *dst_size);
                ctx->prev_pkt_size = *dst_size;
            }
            /* Ring buffer update */
            decomp_ring_append(ctx, (const uint8_t *)dst, *dst_size);

            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_decompressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            ctx->context_seq = (uint8_t)(hdr.context_seq + 1);
            netc_adaptive_update(ctx, (const uint8_t *)dst, *dst_size);
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
    size_t pkt_hdr_sz = 0;
    netc_result_t r = validate_header(src, src_size, dst_cap, &hdr,
                                       0 /* legacy header for stateless */,
                                       &pkt_hdr_sz);
    if (NETC_UNLIKELY(r != NETC_OK)) {
        return r;
    }

    if (!(hdr.flags & NETC_PKT_FLAG_PASSTHRU)) {
        if (NETC_UNLIKELY(hdr.model_id != dict->model_id)) {
            return NETC_ERR_VERSION;
        }
    }

    /* Stateless path has no history — delta-encoded packets cannot be decoded. */
    if (NETC_UNLIKELY(hdr.flags & NETC_PKT_FLAG_DELTA)) {
        return NETC_ERR_CORRUPT;
    }

    const uint8_t *payload = (const uint8_t *)src + pkt_hdr_sz;
    uint8_t alg_id = hdr.algorithm;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS)      alg_id = NETC_ALG_TANS;
    if ((alg_id & 0x0Fu) == NETC_ALG_LZP)       alg_id = NETC_ALG_LZP;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS_PCTX) alg_id = NETC_ALG_TANS_PCTX;
    if ((alg_id & 0x0Fu) == NETC_ALG_TANS_10)   alg_id = NETC_ALG_TANS_10;

    switch (alg_id) {
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
            return decode_tans(dict, dict->tables, &hdr, payload,
                               hdr.compressed_size, dst, dst_size,
                               NULL, 0, 0);

        case NETC_ALG_TANS_PCTX: {
            /* Per-position context-adaptive tANS (stateless path).
             * When BIGRAM flag is set, also switches bigram class per byte. */
            if (hdr.compressed_size < 4) return NETC_ERR_CORRUPT;
            uint32_t initial_state = netc_read_u32_le(payload);
            if (initial_state < NETC_TANS_TABLE_SIZE ||
                initial_state >= 2U * NETC_TANS_TABLE_SIZE)
                return NETC_ERR_CORRUPT;
            const uint8_t *bits = payload + 4;
            size_t bits_sz = hdr.compressed_size - 4;
            netc_bsr_t bsr;
            netc_bsr_init(&bsr, bits, bits_sz);
            int sl_pctx_rc;
            if ((hdr.flags & NETC_PKT_FLAG_BIGRAM) &&
                dict->bigram_tables[0][0].valid) {
                sl_pctx_rc = netc_tans_decode_pctx_bigram(
                    dict->bigram_tables, dict->tables,
                    dict->bigram_class_map,
                    &bsr, (uint8_t *)dst, hdr.original_size, initial_state);
            } else {
                sl_pctx_rc = netc_tans_decode_pctx(
                    dict->tables, &bsr,
                    (uint8_t *)dst, hdr.original_size, initial_state);
            }
            if (sl_pctx_rc != 0)
                return NETC_ERR_CORRUPT;
            *dst_size = hdr.original_size;
            /* LZP XOR inverse: upper nibble of algorithm byte signals LZP */
            if ((hdr.algorithm & 0xF0u) != 0 &&
                dict->lzp_table != NULL)
            {
                netc_lzp_xor_unfilter((const uint8_t *)dst, *dst_size,
                                       dict->lzp_table, (uint8_t *)dst);
            }
            return NETC_OK;
        }

        case NETC_ALG_LZP: {
            /* LZP XOR + tANS: tANS decode then LZP XOR inverse */
            if (dict->lzp_table == NULL)
                return NETC_ERR_DICT_INVALID;
            r = decode_tans(dict, dict->tables, &hdr, payload,
                            hdr.compressed_size, dst, dst_size,
                            NULL, 0, 0);
            if (r != NETC_OK) return r;
            netc_lzp_xor_unfilter((const uint8_t *)dst, *dst_size,
                                   dict->lzp_table, (uint8_t *)dst);
            return NETC_OK;
        }

        case NETC_ALG_TANS_10: {
            /* 10-bit tANS: stateless path.
             * Wire format: [2B state (uint16 LE)][bitstream].
             * Rescale 12-bit freq table to 10-bit, build table, decode. */
            uint32_t bucket_sl = (uint32_t)(hdr.algorithm >> 4);
            if (bucket_sl >= NETC_CTX_COUNT) bucket_sl = 0;
            const netc_tans_table_t *tbl12_sl = &dict->tables[bucket_sl];
            if (!tbl12_sl->valid) return NETC_ERR_DICT_INVALID;

            if (hdr.compressed_size < 2) return NETC_ERR_CORRUPT;
            uint32_t init_st = (uint32_t)netc_read_u16_le(payload);
            if (init_st < NETC_TANS_TABLE_SIZE_10 ||
                init_st >= 2U * NETC_TANS_TABLE_SIZE_10)
                return NETC_ERR_CORRUPT;

            netc_freq_table_t freq10_sl;
            if (netc_freq_rescale_12_to_10(&tbl12_sl->freq, &freq10_sl) != 0)
                return NETC_ERR_DICT_INVALID;
            netc_tans_table_10_t tbl10_sl;
            if (netc_tans_build_10(&tbl10_sl, &freq10_sl) != 0)
                return NETC_ERR_DICT_INVALID;

            const uint8_t *bits_sl = payload + 2;
            size_t bits_sz_sl = hdr.compressed_size - 2;
            netc_bsr_t bsr_sl;
            netc_bsr_init(&bsr_sl, bits_sl, bits_sz_sl);
            if (netc_tans_decode_10(&tbl10_sl, &bsr_sl, (uint8_t *)dst,
                                     hdr.original_size, init_st) != 0)
                return NETC_ERR_CORRUPT;
            *dst_size = hdr.original_size;
            return NETC_OK;
        }

        case NETC_ALG_RANS:
            return NETC_ERR_UNSUPPORTED;

        default:
            return NETC_ERR_CORRUPT;
    }
}
