/**
 * netc_compress.c — Compression entry point.
 *
 * Phase 2: tANS compression with passthrough fallback (AD-006).
 * Phase 3: Field-class-aware delta pre-pass (AD-002).
 *
 *   - Validates all arguments.
 *   - If NETC_CFG_FLAG_DELTA is set and a prior packet exists, applies delta
 *     encoding (field-class aware, AD-002) before tANS.
 *   - If a dictionary with valid tANS tables is present, attempts tANS encoding
 *     per context bucket (RFC-001 §6.2).
 *   - Falls back to passthrough if compressed_size >= original_size (AD-006).
 *   - Updates statistics if NETC_CFG_FLAG_STATS is set.
 *
 * Packet layout for tANS (algorithm = NETC_ALG_TANS):
 *   [header  8 bytes ]
 *   [initial_state 4 bytes  LE — encoder final state for decoder init]
 *   [bitstream payload — variable length]
 *
 * Delta is indicated by NETC_PKT_FLAG_DELTA in the header flags field.
 * The decompressor applies the inverse pass after decoding.
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
 * Internal: bucket offset boundaries
 *
 * Given a bucket index b, return the first byte offset that falls in it.
 * This mirrors the inverse of netc_ctx_bucket().
 * ========================================================================= */
static uint32_t bucket_start_offset(uint32_t b) {
    static const uint32_t starts[NETC_CTX_COUNT] = {
           0,    8,   16,   24,   32,   48,   64,   96,
         128,  192,  256,  384,  512, 1024, 4096, 16384
    };
    return (b < NETC_CTX_COUNT) ? starts[b] : 65536U;
}

/* =========================================================================
 * Internal: RLE detection and encoding
 *
 * Encodes runs of identical bytes as (count, symbol) pairs where count is
 * 1–255. If total RLE output >= src_size the caller should skip RLE.
 *
 * Returns the number of bytes written to dst_rle, or (size_t)-1 on failure.
 * ========================================================================= */
static size_t rle_encode(const uint8_t *src, size_t src_size,
                          uint8_t *dst_rle,  size_t rle_cap)
{
    size_t out = 0;
    size_t i   = 0;
    while (i < src_size) {
        uint8_t sym   = src[i];
        size_t  run   = 1;
        while (run < 255 && i + run < src_size && src[i + run] == sym)
            run++;
        if (out + 2 > rle_cap) return (size_t)-1;
        dst_rle[out++] = (uint8_t)run;
        dst_rle[out++] = sym;
        i += run;
    }
    return out;
}

/* =========================================================================
 * Internal: LZ77 encode — O(n) hash-accelerated
 *
 * Token stream (no external dictionary; back-references within same packet):
 *   Literal run:  [0lllllll]  len = bits[6:0]+1 (1–128 raw bytes follow)
 *   Back-ref:     [1lllllll][oooooooo]
 *                   match_len    = bits[6:0]+3  (3–130)
 *                   match_offset = byte+1       (1–256 bytes back)
 *
 * Search strategy: Hash chain on 3-byte prefix (FNV-1a mod 1024).
 * Each position is looked up via a hash table pointing to the last seen
 * occurrence of the same 3-byte pattern within the last 256 bytes.
 * This makes the expected match-finding cost O(1) per byte.
 *
 * Minimum match length is 3 (a 2-byte token saving 0 net bytes vs 2 literals).
 *
 * Returns bytes written to dst_lz, or (size_t)-1 if lz >= src_size.
 * ========================================================================= */
#define LZ77_HT_SIZE  1024u
#define LZ77_HT_MASK  (LZ77_HT_SIZE - 1u)

static NETC_INLINE uint32_t lz77_hash3(const uint8_t *p) {
    /* FNV-1a on 3 bytes, folded to LZ77_HT_SIZE */
    uint32_t h = 2166136261u;
    h ^= p[0]; h *= 16777619u;
    h ^= p[1]; h *= 16777619u;
    h ^= p[2]; h *= 16777619u;
    return h & LZ77_HT_MASK;
}

static size_t lz77_encode(const uint8_t *src, size_t src_size,
                           uint8_t *dst_lz, size_t lz_cap)
{
    /* Hash table: index → last position with that 3-byte hash.
     * We use (size_t)-1 as sentinel for "empty". */
    size_t ht[LZ77_HT_SIZE];
    for (size_t k = 0; k < LZ77_HT_SIZE; k++) ht[k] = (size_t)-1;

    size_t out      = 0;
    size_t i        = 0;
    size_t lit_start = 0; /* start of pending literal run */

    /* Helper lambda via local block: flush pending literals [lit_start, end) */
#define LZ77_FLUSH_LITS(end) do { \
    size_t _ls = lit_start, _le = (end); \
    while (_ls < _le) { \
        size_t _ll = _le - _ls; if (_ll > 128) _ll = 128; \
        if (out + 1 + _ll > lz_cap) return (size_t)-1; \
        dst_lz[out++] = (uint8_t)(_ll - 1); \
        memcpy(dst_lz + out, src + _ls, _ll); \
        out += _ll; _ls += _ll; \
    } \
} while (0)

    while (i + 3 <= src_size) {
        uint32_t h   = lz77_hash3(src + i);
        size_t   pos = ht[h];
        ht[h] = i; /* always update to most recent */

        /* Check if candidate is within 256-byte window and actually matches */
        size_t best_len = 0;
        size_t best_off = 0;
        if (pos != (size_t)-1 && i - pos <= 256) {
            size_t off  = i - pos;
            size_t max_m = src_size - i;
            if (max_m > 130) max_m = 130;
            size_t mlen = 0;
            while (mlen < max_m && src[pos + mlen] == src[i + mlen])
                mlen++;
            if (mlen >= 3) {
                best_len = mlen;
                best_off = off;
            }
        }

        if (best_len >= 3) {
            /* Flush pending literals first */
            LZ77_FLUSH_LITS(i);
            if (out >= src_size) return (size_t)-1;
            /* Emit back-reference */
            if (out + 2 > lz_cap) return (size_t)-1;
            dst_lz[out++] = (uint8_t)(0x80u | (uint8_t)(best_len - 3));
            dst_lz[out++] = (uint8_t)(best_off - 1);
            /* Update hash table for skipped positions */
            for (size_t k = 1; k < best_len && i + k + 3 <= src_size; k++)
                ht[lz77_hash3(src + i + k)] = i + k;
            i        += best_len;
            lit_start = i;
        } else {
            i++; /* accumulate literal */
        }

        if (out >= src_size) return (size_t)-1;
    }

    /* Flush remaining literals (including tail < 3 bytes) */
    if (i < src_size) {
        /* Update hash for any remaining positions we skipped */
        i = src_size; /* just point past end */
    }
    LZ77_FLUSH_LITS(src_size);

#undef LZ77_FLUSH_LITS

    return (out < src_size) ? out : (size_t)-1;
}

/* =========================================================================
 * Internal: select tANS table — unigram or bigram sub-table.
 *
 * When ctx_flags has NETC_CFG_FLAG_BIGRAM set, returns the bigram sub-table
 * for bucket `bucket` and bigram class derived from `prev_byte`.
 * Otherwise returns the unigram table for `bucket`.
 * ========================================================================= */
static NETC_INLINE const netc_tans_table_t *
select_tans_table(const netc_dict_t *dict, uint32_t bucket,
                  uint8_t prev_byte, uint32_t ctx_flags)
{
    if (ctx_flags & NETC_CFG_FLAG_BIGRAM) {
        uint32_t bclass = netc_bigram_class(prev_byte);
        const netc_tans_table_t *tbl = &dict->bigram_tables[bucket][bclass];
        if (tbl->valid) return tbl;
    }
    return &dict->tables[bucket];
}

/* =========================================================================
 * Internal: single-region tANS encode (legacy format: [4B state][bitstream])
 *
 * Encodes all src bytes using the table for the bucket of byte 0.
 * When NETC_CFG_FLAG_BIGRAM is set, selects the bigram sub-table using
 * the implicit start-of-packet previous byte (0x00).
 *
 * Sets *used_mreg_flag = 0.
 * Returns 0 on success (sets *compressed_payload_size), -1 on failure.
 * ========================================================================= */
static int try_tans_single_region(
    const netc_dict_t *dict,
    const uint8_t     *src,
    size_t             src_size,
    uint8_t           *dst,
    size_t             dst_payload_cap,
    size_t            *compressed_payload_size,
    int               *used_mreg_flag,
    int               *used_x2_flag,
    uint32_t           ctx_flags)
{
    uint32_t bucket = netc_ctx_bucket(0);
    /* For single-region, prev_byte at position 0 is implicitly 0x00 (packet start) */
    const netc_tans_table_t *tbl = select_tans_table(dict, bucket, 0x00u, ctx_flags);
    if (!tbl->valid) return -1;

    /* Use dual-interleaved (x2) encode for regions >= 8 bytes.
     * x2 exposes ILP (two independent ANS states) at the cost of 4 extra
     * header bytes (8B total vs 4B). Only worth it when bitstream savings
     * exceed the extra header bytes — guaranteed for src_size >= 8.
     * Note: x2 is disabled for bigram (bigram adds NETC_PKT_FLAG_BIGRAM
     * and uses single-state for simpler decoder logic). */
    if (!(ctx_flags & NETC_CFG_FLAG_BIGRAM) && src_size >= 8 && dst_payload_cap >= 8) {
        netc_bsw_t bsw;
        netc_bsw_init(&bsw, dst + 8, dst_payload_cap - 8);

        uint32_t state0 = 0, state1 = 0;
        if (netc_tans_encode_x2(tbl, src, src_size, &bsw, &state0, &state1) == 0) {
            size_t bs = netc_bsw_flush(&bsw);
            if (bs != (size_t)-1) {
                netc_write_u32_le(dst,     state0);
                netc_write_u32_le(dst + 4, state1);
                *compressed_payload_size = 8u + bs;
                *used_mreg_flag = 0;
                *used_x2_flag   = 1;
                return 0;
            }
        }
    }

    /* Fallback: single-state encode (4B header) */
    if (dst_payload_cap < 4) return -1;

    netc_bsw_t bsw;
    netc_bsw_init(&bsw, dst + 4, dst_payload_cap - 4);

    uint32_t final_state = netc_tans_encode(tbl, src, src_size,
                                            &bsw, NETC_TANS_TABLE_SIZE);
    if (final_state == 0) return -1;

    size_t bs = netc_bsw_flush(&bsw);
    if (bs == (size_t)-1) return -1;

    netc_write_u32_le(dst, final_state);
    *compressed_payload_size = 4u + bs;
    *used_mreg_flag = 0;
    *used_x2_flag   = 0;
    return 0;
}

/* =========================================================================
 * Internal: multi-region tANS compress (v0.2)
 *
 * Encodes each contiguous bucket region as an independent ANS stream.
 * Wire format (after the 8-byte packet header):
 *   [1B]      n_regions
 *   [n×8B]    descriptors — per region: {uint32_le state, uint32_le bs_bytes}
 *   [N B]     bitstreams  — concatenated region bitstreams (region 0 first)
 *
 * Returns 0 on success (sets *compressed_payload_size, *used_mreg_flag=1).
 * Returns -1 if any table is invalid or output expands.
 * ========================================================================= */
static int try_tans_compress(
    const netc_dict_t *dict,
    const uint8_t     *src,
    size_t             src_size,
    uint8_t           *dst,             /* points past the 8-byte packet header */
    size_t             dst_payload_cap,
    size_t            *compressed_payload_size,
    int               *used_mreg_flag,  /* out: 1 if MREG format was used */
    int               *used_x2_flag,   /* out: 1 if dual-state x2 encode was used */
    uint32_t           ctx_flags)       /* NETC_CFG_FLAG_* bitmask */
{
    if (src_size == 0) return -1;

    *used_x2_flag = 0;

    uint32_t first_bucket = netc_ctx_bucket(0);
    uint32_t last_bucket  = netc_ctx_bucket((uint32_t)(src_size - 1));
    uint32_t n_regions    = last_bucket - first_bucket + 1;

    /* For single-bucket packets use the simpler legacy format (less overhead) */
    if (n_regions == 1) {
        return try_tans_single_region(dict, src, src_size, dst, dst_payload_cap,
                                      compressed_payload_size, used_mreg_flag,
                                      used_x2_flag, ctx_flags);
    }

    /* Validate all per-bucket tables */
    for (uint32_t b = first_bucket; b <= last_bucket; b++) {
        if (!dict->tables[b].valid) return -1;
    }

    /* Layout: 1B n_regions + n_regions * 8B descriptors */
    size_t hdr_bytes = 1u + (size_t)n_regions * 8u;
    if (dst_payload_cap < hdr_bytes) return -1;

    /* If MREG header overhead is too large relative to packet size, use single-region */
    if (hdr_bytes >= src_size || hdr_bytes * 4u >= src_size) {
        return try_tans_single_region(dict, src, src_size, dst, dst_payload_cap,
                                      compressed_payload_size, used_mreg_flag,
                                      used_x2_flag, ctx_flags);
    }

    uint8_t *bits_base = dst + hdr_bytes;
    size_t   bits_cap  = dst_payload_cap - hdr_bytes;
    size_t   bits_used = 0;

    /* Encode each region into the bitstream buffer (in order).
     * For bigram encoding, each region's table is selected using the last byte
     * of the preceding region (or 0x00 for the first region). */
    uint8_t region_prev_byte = 0x00u; /* implicit start-of-packet */

    for (uint32_t r = 0; r < n_regions; r++) {
        uint32_t bucket      = first_bucket + r;
        uint32_t rstart      = bucket_start_offset(bucket);
        uint32_t rend_bound  = bucket_start_offset(bucket + 1);
        size_t   region_start = (size_t)rstart;
        size_t   region_end   = (rend_bound < (uint32_t)src_size)
                                    ? (size_t)rend_bound : src_size;
        size_t   region_len   = region_end - region_start;

        if (region_len == 0) {
            /* Empty region — write sentinel zeros */
            netc_write_u32_le(dst + 1u + r * 8u,     0U);
            netc_write_u32_le(dst + 1u + r * 8u + 4u, 0U);
            continue;
        }

        const netc_tans_table_t *tbl = select_tans_table(dict, bucket,
                                                          region_prev_byte,
                                                          ctx_flags);

        netc_bsw_t bsw;
        netc_bsw_init(&bsw, bits_base + bits_used, bits_cap - bits_used);

        uint32_t final_state = netc_tans_encode(
            tbl,
            src + region_start, region_len,
            &bsw, NETC_TANS_TABLE_SIZE);

        if (final_state == 0) return -1;

        size_t region_bs = netc_bsw_flush(&bsw);
        if (region_bs == (size_t)-1) return -1;
        if (bits_used + region_bs > bits_cap) return -1;

        netc_write_u32_le(dst + 1u + r * 8u,      final_state);
        netc_write_u32_le(dst + 1u + r * 8u + 4u, (uint32_t)region_bs);
        bits_used += region_bs;

        /* Update prev_byte for next region (last byte of this region) */
        region_prev_byte = src[region_end - 1];
    }

    dst[0] = (uint8_t)n_regions;
    *compressed_payload_size = hdr_bytes + bits_used;
    *used_mreg_flag = 1;
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

    /* -----------------------------------------------------------------------
     * Phase 3: Delta pre-pass (AD-002, field-class-aware)
     *
     * Conditions for delta:
     *   - NETC_CFG_FLAG_DELTA is set in context flags
     *   - A previous packet exists (prev_pkt_size > 0) with matching size
     *   - Current packet is large enough to benefit (>= NETC_DELTA_MIN_SIZE)
     *
     * We write delta residuals into the arena, then compress the residuals.
     * If the previous packet size differs, we fall back to no-delta for this
     * packet (size mismatch makes prediction less useful anyway).
     * ----------------------------------------------------------------------- */
    const uint8_t *compress_src = (const uint8_t *)src;
    uint8_t        pkt_flags    = NETC_PKT_FLAG_DICT_ID;
    int            did_delta    = 0;

    if ((ctx->flags & NETC_CFG_FLAG_DELTA) &&
        ctx->prev_pkt_size == src_size &&
        src_size >= NETC_DELTA_MIN_SIZE &&
        ctx->arena_size >= src_size)
    {
        /* Encode residuals into arena via SIMD dispatch */
        ctx->simd_ops.delta_encode(ctx->prev_pkt, (const uint8_t *)src,
                                   ctx->arena, src_size);
        compress_src = ctx->arena;
        pkt_flags   |= NETC_PKT_FLAG_DELTA;
        did_delta    = 1;
    }

    /* Attempt tANS first if we have a valid dictionary */
    if (dict != NULL && src_size > 0) {
        size_t payload_cap = dst_cap - NETC_HEADER_SIZE;
        uint8_t *payload   = (uint8_t *)dst + NETC_HEADER_SIZE;
        size_t  compressed_payload = 0;
        int     used_mreg = 0;
        int     used_x2   = 0;

        if (try_tans_compress(dict, compress_src, src_size,
                              payload, payload_cap,
                              &compressed_payload, &used_mreg, &used_x2,
                              ctx->flags) == 0 &&
            compressed_payload < src_size) {

            /* tANS compressed — check if LZ77 would do better.
             * Only try LZ77 when tANS ratio > 0.5 (high-redundancy data).
             *
             * Case A: !did_delta — arena is free; LZ77 encodes there.
             *   If LZ77 wins, copy from arena to dst payload (tANS already there).
             *
             * Case B: did_delta — arena holds delta residuals (compress_src IS arena).
             *   LZ77 must encode into dst payload (overwriting tANS result).
             *   If LZ77 wins: emit LZ77+DELTA packet.
             *   If LZ77 loses: re-run tANS into dst payload.
             *   Re-run cost is bounded: only attempted for small packets (≤1024B)
             *   where LZ77 probe is fast and tANS re-run is cheap. */
            if (compressed_payload * 2 > src_size) {
                if (!did_delta && ctx->arena_size >= src_size) {
                    /* Case A: LZ77 into arena, tANS stays in dst payload */
                    size_t lz_len = lz77_encode(compress_src, src_size,
                                                ctx->arena, ctx->arena_size);
                    if (lz_len < compressed_payload && lz_len < src_size &&
                        NETC_HEADER_SIZE + lz_len <= dst_cap) {
                        /* LZ77 wins: copy from arena to dst payload */
                        memcpy((uint8_t *)dst + NETC_HEADER_SIZE, ctx->arena, lz_len);
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lz_len;
                        hdr.flags           = pkt_flags | NETC_PKT_FLAG_LZ77
                                              | NETC_PKT_FLAG_PASSTHRU;
                        hdr.algorithm       = NETC_ALG_PASSTHRU;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_write(dst, &hdr);
                        *dst_size = NETC_HEADER_SIZE + lz_len;
                        if (ctx->prev_pkt != NULL) {
                            memcpy(ctx->prev_pkt, src, src_size);
                            ctx->prev_pkt_size = src_size;
                        }
                        if (ctx->flags & NETC_CFG_FLAG_STATS) {
                            ctx->stats.packets_compressed++;
                            ctx->stats.bytes_in  += src_size;
                            ctx->stats.bytes_out += *dst_size;
                            ctx->stats.passthrough_count++;
                        }
                        return NETC_OK;
                    }
                    /* LZ77 didn't beat tANS — tANS payload in dst is still valid */
                } else if (did_delta && src_size <= 1024) {
                    /* Case B: arena holds delta residuals, so we can't use it for LZ77.
                     * Quick redundancy check: count distinct byte values in the first
                     * 32 bytes.  ≤ 4 distinct values → runs/periodic patterns that LZ77
                     * compresses well.  > 4 → diverse residuals (game-state WL-001/002/003)
                     * that LZ77 won't beat tANS → skip to avoid overhead. */
                    {
                        size_t scan  = src_size < 32u ? src_size : 32u;
                        uint8_t seen_arr[32];
                        unsigned n_uniq = 0;
                        unsigned too_diverse = 0;
                        for (size_t _q = 0; _q < scan; _q++) {
                            uint8_t b = compress_src[_q];
                            unsigned found = 0;
                            for (unsigned _u = 0; _u < n_uniq; _u++) {
                                if (seen_arr[_u] == b) { found = 1; break; }
                            }
                            if (!found) {
                                seen_arr[n_uniq++] = b;
                                if (n_uniq > 4) { too_diverse = 1; break; }
                            }
                        }
                        if (too_diverse) goto case_b_skip;
                    }
                    /* Save tANS output to stack buffer, then try LZ77 into dst payload.
                     * If LZ77 loses, restore from stack (cheap memcpy, no re-encode). */
                    uint8_t tans_save[1024];
                    memcpy(tans_save, payload, compressed_payload);
                    int    tans_used_mreg_save = used_mreg;
                    int    tans_used_x2_save   = used_x2;
                    size_t tans_cp_save        = compressed_payload;

                    size_t lz_len = lz77_encode(compress_src, src_size,
                                                payload, payload_cap);
                    if (lz_len < tans_cp_save && lz_len < src_size &&
                        NETC_HEADER_SIZE + lz_len <= dst_cap) {
                        /* LZ77 wins */
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lz_len;
                        hdr.flags           = pkt_flags | NETC_PKT_FLAG_LZ77
                                              | NETC_PKT_FLAG_PASSTHRU;
                        hdr.algorithm       = NETC_ALG_PASSTHRU;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_write(dst, &hdr);
                        *dst_size = NETC_HEADER_SIZE + lz_len;
                        if (ctx->prev_pkt != NULL) {
                            memcpy(ctx->prev_pkt, src, src_size);
                            ctx->prev_pkt_size = src_size;
                        }
                        if (ctx->flags & NETC_CFG_FLAG_STATS) {
                            ctx->stats.packets_compressed++;
                            ctx->stats.bytes_in  += src_size;
                            ctx->stats.bytes_out += *dst_size;
                            ctx->stats.passthrough_count++;
                        }
                        return NETC_OK;
                    }
                    /* LZ77 lost — restore tANS output from stack */
                    memcpy(payload, tans_save, tans_cp_save);
                    compressed_payload = tans_cp_save;
                    used_mreg = tans_used_mreg_save;
                    used_x2   = tans_used_x2_save;
                    goto case_b_skip;
                }
case_b_skip:;
            }

            /* tANS wins */
            uint8_t extra_flags = (used_mreg ? NETC_PKT_FLAG_MREG   : 0)
                                | (used_x2   ? NETC_PKT_FLAG_X2     : 0)
                                | ((ctx->flags & NETC_CFG_FLAG_BIGRAM) ? NETC_PKT_FLAG_BIGRAM : 0);
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)compressed_payload;
            hdr.flags           = pkt_flags | extra_flags;
            hdr.algorithm       = NETC_ALG_TANS;
            hdr.model_id        = dict->model_id;
            hdr.context_seq     = seq;
            netc_hdr_write(dst, &hdr);
            *dst_size = NETC_HEADER_SIZE + compressed_payload;
            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, src, src_size);
                ctx->prev_pkt_size = src_size;
            }
            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_compressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
            }
            return NETC_OK;
        }
    }

    /* --- LZ77 path: tANS failed/unavailable or didn't compress.
     * Encode LZ77 into dst payload area.
     * When did_delta=1, pkt_flags has NETC_PKT_FLAG_DELTA; the decompressor
     * handles PASSTHRU+DELTA+LZ77 via its delta post-pass. */
    if (src_size > 0 && dst_cap > NETC_HEADER_SIZE) {
        uint8_t *out_payload = (uint8_t *)dst + NETC_HEADER_SIZE;
        size_t   out_cap     = dst_cap - NETC_HEADER_SIZE;
        size_t   lz_len      = lz77_encode(compress_src, src_size,
                                           out_payload, out_cap);
        if (lz_len == (size_t)-1 || lz_len >= src_size) goto lz77_failed;
        {
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)lz_len;
            hdr.flags           = pkt_flags | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_PASSTHRU;
            hdr.algorithm       = NETC_ALG_PASSTHRU;
            hdr.model_id        = (dict != NULL) ? dict->model_id : 0;
            hdr.context_seq     = seq;
            netc_hdr_write(dst, &hdr);
            *dst_size = NETC_HEADER_SIZE + lz_len;

            if (ctx->prev_pkt != NULL) {
                memcpy(ctx->prev_pkt, src, src_size);
                ctx->prev_pkt_size = src_size;
            }
            if (ctx->flags & NETC_CFG_FLAG_STATS) {
                ctx->stats.packets_compressed++;
                ctx->stats.bytes_in  += src_size;
                ctx->stats.bytes_out += *dst_size;
                ctx->stats.passthrough_count++;
            }
            return NETC_OK;
        }
    }

lz77_failed:
    /* If we ran delta but neither tANS nor LZ77 compressed, update prev_pkt. */
    (void)did_delta;
    if (ctx->prev_pkt != NULL) {
        memcpy(ctx->prev_pkt, src, src_size);
        ctx->prev_pkt_size = src_size;
    }

    /* Fall back to passthrough (no delta flag — raw bytes in payload) */
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
        int     used_mreg  = 0;
        int     used_x2    = 0;

        int tans_ok = (try_tans_compress(dict, (const uint8_t *)src, src_size,
                                         payload, payload_cap,
                                         &compressed_payload, &used_mreg, &used_x2,
                                         0U /* stateless: no bigram */) == 0
                       && compressed_payload < src_size);

        if (tans_ok) {
            /* tANS succeeded — check if LZ77 beats it (ratio > 0.5 threshold).
             * Stateless has no arena; use a small stack buffer capped at 1024B.
             * For larger packets only tANS is tried (stack budget constraint). */
            if (compressed_payload * 2 > src_size && src_size <= 1024) {
                uint8_t lz_buf[1024];
                size_t lz_len = lz77_encode((const uint8_t *)src, src_size,
                                            lz_buf, sizeof(lz_buf));
                if (lz_len < compressed_payload && lz_len < src_size &&
                    NETC_HEADER_SIZE + lz_len <= dst_cap) {
                    memcpy(payload, lz_buf, lz_len);
                    netc_pkt_header_t hdr;
                    hdr.original_size   = (uint16_t)src_size;
                    hdr.compressed_size = (uint16_t)lz_len;
                    hdr.flags           = NETC_PKT_FLAG_DICT_ID
                                        | NETC_PKT_FLAG_LZ77
                                        | NETC_PKT_FLAG_PASSTHRU;
                    hdr.algorithm       = NETC_ALG_PASSTHRU;
                    hdr.model_id        = dict->model_id;
                    hdr.context_seq     = 0;
                    netc_hdr_write(dst, &hdr);
                    *dst_size = NETC_HEADER_SIZE + lz_len;
                    return NETC_OK;
                }
            }

            /* tANS wins */
            uint8_t extra_flags = (used_mreg ? NETC_PKT_FLAG_MREG : 0)
                                | (used_x2   ? NETC_PKT_FLAG_X2   : 0);
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)compressed_payload;
            hdr.flags           = NETC_PKT_FLAG_DICT_ID | extra_flags;
            hdr.algorithm       = NETC_ALG_TANS;
            hdr.model_id        = dict->model_id;
            hdr.context_seq     = 0;
            netc_hdr_write(dst, &hdr);
            *dst_size = NETC_HEADER_SIZE + compressed_payload;
            return NETC_OK;
        }

        /* tANS failed — try LZ77 directly into payload */
        if (src_size > 0) {
            size_t lz_len = lz77_encode((const uint8_t *)src, src_size,
                                        payload, payload_cap);
            if (lz_len != (size_t)-1 && lz_len < src_size &&
                NETC_HEADER_SIZE + lz_len <= dst_cap) {
                netc_pkt_header_t hdr;
                hdr.original_size   = (uint16_t)src_size;
                hdr.compressed_size = (uint16_t)lz_len;
                hdr.flags           = NETC_PKT_FLAG_DICT_ID
                                    | NETC_PKT_FLAG_LZ77
                                    | NETC_PKT_FLAG_PASSTHRU;
                hdr.algorithm       = NETC_ALG_PASSTHRU;
                hdr.model_id        = dict->model_id;
                hdr.context_seq     = 0;
                netc_hdr_write(dst, &hdr);
                *dst_size = NETC_HEADER_SIZE + lz_len;
                return NETC_OK;
            }
        }
    }

    return emit_passthrough(NULL, dict, src, src_size, dst, dst_cap, dst_size, 0);
}
