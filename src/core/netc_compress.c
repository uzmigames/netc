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
#include <limits.h>

/* Internal-only flag: suppress X2 dual-state encoding.
 * Used when LZP compact types are active — the compact packet type table
 * for LZP (0x70-0x8F) cannot represent X2, so encoding must be single-state. */
#define NETC_INTERNAL_NO_X2  (1U << 31)

/* Internal-only flag: skip single-region comparison in try_tans_compress().
 * When set, PCTX is used directly for multi-bucket packets without trying
 * single-region best-fit tables. Set when the input is delta residuals or
 * LZP-filtered data — both have position-specific byte distributions that
 * PCTX (per-position tables) handles better than any single-region table.
 * Expected ratio regression: <1% (PCTX dominates for structured pre-filtered data). */
#define NETC_INTERNAL_SKIP_SR (1U << 30)

/* =========================================================================
 * Internal: FNV-1a hash of 3 bytes folded to 4096 (for ring_ht)
 * ========================================================================= */
NETC_MAYBE_UNUSED static NETC_INLINE uint32_t ring_ht_hash3(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    h ^= p[0]; h *= 16777619u;
    h ^= p[1]; h *= 16777619u;
    h ^= p[2]; h *= 16777619u;
    return h & 4095u; /* ring_ht_size - 1 */
}

/* =========================================================================
 * Internal: append raw bytes to the context's ring buffer (circular)
 * ========================================================================= */
static void ctx_ring_append(netc_ctx_t *ctx,
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
    uint8_t            context_seq,
    int                compact)
{
    netc_pkt_header_t hdr;
    hdr.original_size   = (uint16_t)src_size;
    hdr.compressed_size = (uint16_t)src_size;
    hdr.flags           = NETC_PKT_FLAG_PASSTHRU | NETC_PKT_FLAG_DICT_ID;
    hdr.algorithm       = NETC_ALG_PASSTHRU;
    hdr.model_id        = (dict != NULL) ? dict->model_id : 0;
    hdr.context_seq     = context_seq;

    uint8_t *out = (uint8_t *)dst;
    size_t hdr_sz = netc_hdr_emit(out, &hdr, compact);
    size_t out_size = hdr_sz + src_size;

    if (NETC_UNLIKELY(dst_cap < out_size)) {
        return NETC_ERR_BUF_SMALL;
    }

    memcpy(out + hdr_sz, src, src_size);

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
NETC_MAYBE_UNUSED static uint32_t bucket_start_offset(uint32_t b) {
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
NETC_MAYBE_UNUSED static size_t rle_encode(const uint8_t *src, size_t src_size,
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
 * Internal: cross-packet LZ77 encode — ring buffer + within-packet history
 *
 * Token stream:
 *   [0lllllll]                     literal run: len=bits[6:0]+1 (1–128)
 *   [10llllll][oooooooo]            short back-ref: len=bits[5:0]+3, offset=byte+1 (1–256)
 *                                   offset counted back from current OUTPUT position only
 *   [11llllll][lo][hi]              long back-ref: len=bits[5:0]+3, offset=u16le+1 (1–65536)
 *                                   offset is into ring buffer: ring[(ring_write - offset) % ring_size]
 *
 * ring / ring_size / ring_pos describe the encoder's circular history buffer.
 * ring_pos is the next write position (last appended packet ends at ring_pos-1).
 *
 * Returns bytes written to dst_lz, or (size_t)-1 if lz >= src_size.
 * ========================================================================= */
#define LZ77X_MAX_LONG_OFFSET 65536u
#define LZ77X_HT_SIZE         4096u
#define LZ77X_HT_MASK         (LZ77X_HT_SIZE - 1u)

/* FNV-1a on 3 bytes, folded to LZ77X_HT_SIZE */
static NETC_INLINE uint32_t lz77x_hash3(const uint8_t *p) {
    uint32_t h = 2166136261u;
    h ^= p[0]; h *= 16777619u;
    h ^= p[1]; h *= 16777619u;
    h ^= p[2]; h *= 16777619u;
    return h & LZ77X_HT_MASK;
}

/* =========================================================================
 * Internal: cross-packet LZ77 encode with on-demand ring hash table
 *
 * Builds a local ring hash table from the most recent bytes in the ring
 * buffer (up to prev_pkt_size bytes before ring_pos). This avoids O(N)
 * per-packet hash maintenance overhead — cost is only paid when LZ77X
 * is actually attempted.
 *
 * Uses two hash tables:
 *   ring_ht[4096]: local stack — maps hash → absolute ring pos
 *   src_ht[4096]:  local stack — maps hash → src position (within-packet)
 *
 * Returns bytes written to dst_lz, or (size_t)-1 if lz >= src_size.
 * ========================================================================= */
static size_t lz77x_encode(
    const uint8_t *src,         size_t src_size,
    const uint8_t *ring,        uint32_t ring_size, uint32_t ring_pos,
    uint32_t       prev_pkt_size,  /* how many recent ring bytes to hash */
    uint8_t       *dst_lz,      size_t lz_cap)
{
    if (ring == NULL || ring_size == 0) {
        return lz77_encode(src, src_size, dst_lz, lz_cap);
    }

    /* Build local ring hash table from the most recent bytes in the ring.
     * Scan up to prev_pkt_size bytes (the last packet appended to the ring),
     * capped at ring_size - 3 to avoid scanning the entire 64KB ring. */
    int32_t ring_ht[LZ77X_HT_SIZE];
    for (size_t k = 0; k < LZ77X_HT_SIZE; k++) ring_ht[k] = INT32_MIN;

    {
        uint32_t scan_len = prev_pkt_size;
        if (scan_len > ring_size - 2) scan_len = ring_size - 2;
        if (scan_len < 3) scan_len = 0;
        /* Scan the most recent scan_len bytes ending at ring_pos */
        uint32_t scan_start = (ring_pos + ring_size - scan_len) % ring_size;
        for (uint32_t j = 0; j + 2 < scan_len; j++) {
            uint32_t abs_pos = (scan_start + j) % ring_size;
            uint8_t b0 = ring[abs_pos];
            uint8_t b1 = ring[(abs_pos + 1) % ring_size];
            uint8_t b2 = ring[(abs_pos + 2) % ring_size];
            uint8_t tmp[3] = {b0, b1, b2};
            uint32_t h = lz77x_hash3(tmp);
            ring_ht[h] = (int32_t)abs_pos;
        }
    }

    /* Local (within-packet) hash table: hash → src position */
    int32_t src_ht[LZ77X_HT_SIZE];
    for (size_t k = 0; k < LZ77X_HT_SIZE; k++) src_ht[k] = INT32_MIN;

    size_t out       = 0;
    size_t i         = 0;
    size_t lit_start = 0;

#define LZ77X_FLUSH_LITS(end) do { \
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
        uint32_t h = lz77x_hash3(src + i);

        /* Check both hash tables — prefer within-packet (shorter ref token) */
        int32_t  ring_entry = ring_ht[h];
        int32_t  src_entry  = src_ht[h];

        /* Update local hash table with current position */
        src_ht[h] = (int32_t)i;

        size_t best_len = 0;
        size_t best_off = 0;
        int    is_long  = 0;

        /* 1) Within-packet candidate (short back-ref, offset 1-256) */
        if (src_entry != INT32_MIN) {
            size_t src_off = i - (size_t)src_entry;
            if (src_off >= 1 && src_off <= 256) {
                size_t max_m = src_size - i;
                if (max_m > 66) max_m = 66;
                size_t mlen = 0;
                const uint8_t *ref = src + src_entry;
                while (mlen < max_m && ref[mlen] == src[i + mlen]) mlen++;
                if (mlen >= 3) { best_len = mlen; best_off = src_off; is_long = 0; }
            }
        }

        /* 2) Ring-buffer candidate (long back-ref): only if better than within-packet */
        if (ring_entry != INT32_MIN) {
            /* Convert absolute ring position to offset from current ring_pos */
            uint32_t abs_pos  = (uint32_t)ring_entry;
            uint32_t ring_off = (ring_pos + ring_size - abs_pos) % ring_size;
            if (ring_off >= 1 && ring_off <= LZ77X_MAX_LONG_OFFSET) {
                size_t max_m = src_size - i;
                if (max_m > 66) max_m = 66;
                size_t mlen = 0;
                uint32_t rstart = (ring_pos + ring_size - ring_off) % ring_size;
                while (mlen < max_m) {
                    if (ring[(rstart + mlen) % ring_size] != src[i + mlen]) break;
                    mlen++;
                }
                /* Accept ring match if it's longer than within-packet match, OR
                 * within-packet match wasn't found. */
                if (mlen >= 3 && mlen > best_len) {
                    best_len = mlen; best_off = ring_off; is_long = 1;
                }
            }
        }

        if (best_len >= 3) {
            LZ77X_FLUSH_LITS(i);
            if (out >= lz_cap) return (size_t)-1;
            if (is_long) {
                if (out + 3 > lz_cap) return (size_t)-1;
                uint16_t off16 = (uint16_t)(best_off - 1u);
                dst_lz[out++] = (uint8_t)(0xC0u | (uint8_t)(best_len - 3));
                dst_lz[out++] = (uint8_t)(off16 & 0xFFu);
                dst_lz[out++] = (uint8_t)(off16 >> 8);
            } else {
                if (out + 2 > lz_cap) return (size_t)-1;
                dst_lz[out++] = (uint8_t)(0x80u | (uint8_t)(best_len - 3));
                dst_lz[out++] = (uint8_t)(best_off - 1u);
            }
            for (size_t k = 1; k < best_len && i + k + 3 <= src_size; k++)
                src_ht[lz77x_hash3(src + i + k)] = (int32_t)(i + k);
            i        += best_len;
            lit_start = i;
        } else {
            i++;
        }

        if (out >= src_size) return (size_t)-1;
    }

    LZ77X_FLUSH_LITS(src_size);

#undef LZ77X_FLUSH_LITS

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
        uint32_t bclass = netc_bigram_class(prev_byte, dict->bigram_class_map);
        const netc_tans_table_t *tbl = &dict->bigram_tables[bucket][bclass];
        if (tbl->valid) return tbl;
    }
    return &dict->tables[bucket];
}

/* =========================================================================
 * Internal: single-region tANS encode with explicit table
 *
 * Encodes all src bytes using the provided table.
 * Sets *used_mreg_flag = 0.
 * Returns compressed payload size on success, (size_t)-1 on failure.
 * ========================================================================= */
static size_t try_tans_single_with_table(
    const netc_tans_table_t *tbl,
    const uint8_t           *src,
    size_t                   src_size,
    uint8_t                 *dst,
    size_t                   dst_payload_cap,
    int                     *used_x2_flag,
    uint32_t                 ctx_flags,
    int                      compact)
{
    if (!tbl->valid) return (size_t)-1;

    /* ANS state fits in 13 bits [4096,8192). In compact mode, write as u16
     * instead of u32, saving 2B per state (2B single, 4B for x2). */
    const size_t state1_sz = compact ? 2u : 4u;
    const size_t state2_sz = compact ? 4u : 8u;

    /* Use dual-interleaved (x2) encode for regions >= 256 bytes.
     * X2 has 2×state overhead vs 1× for single-state — the extra bytes are costly
     * on small packets and the ILP benefit is negligible under 256B. */
    if (!(ctx_flags & NETC_CFG_FLAG_BIGRAM) && !(ctx_flags & NETC_INTERNAL_NO_X2) &&
        src_size >= 256 && dst_payload_cap >= state2_sz) {
        netc_bsw_t bsw;
        netc_bsw_init(&bsw, dst + state2_sz, dst_payload_cap - state2_sz);

        uint32_t state0 = 0, state1 = 0;
        if (netc_tans_encode_x2(tbl, src, src_size, &bsw, &state0, &state1) == 0) {
            size_t bs = netc_bsw_flush(&bsw);
            if (bs != (size_t)-1) {
                if (compact) {
                    netc_write_u16_le(dst,     (uint16_t)state0);
                    netc_write_u16_le(dst + 2, (uint16_t)state1);
                } else {
                    netc_write_u32_le(dst,     state0);
                    netc_write_u32_le(dst + 4, state1);
                }
                *used_x2_flag = 1;
                return state2_sz + bs;
            }
        }
    }

    /* Fallback: single-state encode */
    if (dst_payload_cap < state1_sz) return (size_t)-1;

    netc_bsw_t bsw;
    netc_bsw_init(&bsw, dst + state1_sz, dst_payload_cap - state1_sz);

    uint32_t final_state = netc_tans_encode(tbl, src, src_size,
                                            &bsw, NETC_TANS_TABLE_SIZE);
    if (final_state == 0) return (size_t)-1;

    size_t bs = netc_bsw_flush(&bsw);
    if (bs == (size_t)-1) return (size_t)-1;

    if (compact)
        netc_write_u16_le(dst, (uint16_t)final_state);
    else
        netc_write_u32_le(dst, final_state);
    *used_x2_flag = 0;
    return state1_sz + bs;
}

/* =========================================================================
 * Internal: 10-bit tANS encode with explicit table (small-packet optimization)
 *
 * Encodes all src bytes using the provided 10-bit table.
 * 10-bit state is ALWAYS 2B (uint16, range [1024,2048)).
 * No X2 dual-state for 10-bit (packets are small, ILP benefit negligible).
 * Returns compressed payload size on success, (size_t)-1 on failure.
 * ========================================================================= */
static size_t try_tans_10bit_with_table(
    const netc_tans_table_10_t *tbl,
    const uint8_t              *src,
    size_t                      src_size,
    uint8_t                    *dst,
    size_t                      dst_payload_cap)
{
    if (!tbl->valid) return (size_t)-1;

    /* 10-bit state always fits in 2 bytes [1024, 2048) = 11 bits */
    const size_t state_sz = 2u;
    if (dst_payload_cap < state_sz) return (size_t)-1;

    netc_bsw_t bsw;
    netc_bsw_init(&bsw, dst + state_sz, dst_payload_cap - state_sz);

    uint32_t final_state = netc_tans_encode_10(tbl, src, src_size,
                                                &bsw, NETC_TANS_TABLE_SIZE_10);
    if (final_state == 0) return (size_t)-1;

    size_t bs = netc_bsw_flush(&bsw);
    if (bs == (size_t)-1) return (size_t)-1;

    netc_write_u16_le(dst, (uint16_t)final_state);
    return state_sz + bs;
}

/* =========================================================================
 * Internal: single-region tANS encode (legacy format: [4B state][bitstream])
 *
 * When MREG is not viable (overhead too large), scans all bucket tables that
 * span the packet's byte-offset range and selects the one that produces the
 * smallest compressed output.  The winning bucket index is returned via
 * *out_table_idx so the caller can encode it into the algorithm byte high nibble.
 *
 * When NETC_CFG_FLAG_BIGRAM is set, bigram sub-tables are evaluated for the
 * winning bucket (implicit start-of-packet previous byte 0x00).
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
    uint32_t          *out_table_idx,
    uint32_t           ctx_flags,
    int                compact)
{
    *used_mreg_flag = 0;

    uint32_t first_bucket = netc_ctx_bucket(0);
    uint32_t last_bucket  = netc_ctx_bucket((uint32_t)(src_size > 0 ? src_size - 1 : 0));

    /* For small packets (≤512B) scan all bucket tables covering this packet's
     * offset range and pick the table that yields smallest output.
     * Bigram mode is excluded from multi-scan (no prev_byte continuity). */
    if (!(ctx_flags & NETC_CFG_FLAG_BIGRAM) && src_size <= 512u &&
        last_bucket > first_bucket)
    {
        /* Temporary buffer for trial encoding (stack, bounded by src_size+8) */
        uint8_t trial_buf[512u + 8u];
        size_t  best_cp     = (size_t)-1;
        uint32_t best_bucket = first_bucket;
        int      best_x2     = 0;

        for (uint32_t b = first_bucket; b <= last_bucket; b++) {
            if (!dict->tables[b].valid) continue;
            int trial_x2 = 0;
            size_t cp = try_tans_single_with_table(
                &dict->tables[b], src, src_size,
                trial_buf, sizeof(trial_buf), &trial_x2, ctx_flags, compact);
            if (cp != (size_t)-1 && cp < best_cp) {
                best_cp     = cp;
                best_bucket = b;
                best_x2     = trial_x2;
            }
        }

        if (best_cp == (size_t)-1) return -1;

        /* Re-encode with the best table directly into dst */
        int final_x2 = 0;
        size_t final_cp = try_tans_single_with_table(
            &dict->tables[best_bucket], src, src_size,
            dst, dst_payload_cap, &final_x2, ctx_flags, compact);
        if (final_cp == (size_t)-1) return -1;

        *compressed_payload_size = final_cp;
        *out_table_idx           = best_bucket;
        *used_x2_flag            = final_x2;
        (void)best_x2; /* deterministic re-encode always matches */
        return 0;
    }

    /* Default path: use the table for byte offset 0 */
    uint32_t bucket = first_bucket;
    *out_table_idx  = bucket;

    const netc_tans_table_t *tbl = select_tans_table(dict, bucket, 0x00u, ctx_flags);
    size_t cp = try_tans_single_with_table(tbl, src, src_size, dst,
                                           dst_payload_cap, used_x2_flag, ctx_flags, compact);
    if (cp == (size_t)-1) return -1;
    *compressed_payload_size = cp;
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
    uint8_t           *dst,             /* points past the packet header */
    size_t             dst_payload_cap,
    size_t            *compressed_payload_size,
    int               *used_mreg_flag,  /* out: 1 if MREG format was used */
    int               *used_x2_flag,   /* out: 1 if dual-state x2 encode was used */
    uint32_t          *out_table_idx,  /* out: table bucket used (single-region) */
    uint32_t           ctx_flags,      /* NETC_CFG_FLAG_* bitmask */
    int                compact)        /* compact mode: 2B ANS state instead of 4B */
{
    if (src_size == 0) return -1;

    *used_x2_flag  = 0;
    *out_table_idx = (uint32_t)netc_ctx_bucket(0);

    uint32_t first_bucket = netc_ctx_bucket(0);
    uint32_t last_bucket  = netc_ctx_bucket((uint32_t)(src_size - 1));
    uint32_t n_regions    = last_bucket - first_bucket + 1;

    const size_t state_sz = compact ? 2u : 4u;

    /* For single-bucket packets use the simpler legacy format (less overhead) */
    if (n_regions == 1) {
        return try_tans_single_region(dict, src, src_size, dst, dst_payload_cap,
                                      compressed_payload_size, used_mreg_flag,
                                      used_x2_flag, out_table_idx, ctx_flags,
                                      compact);
    }

    /* Validate all per-bucket tables */
    for (uint32_t b = first_bucket; b <= last_bucket; b++) {
        if (!dict->tables[b].valid) return -1;
    }

    /* --- PCTX path: per-position context-adaptive tANS (v0.4+) ---
     *
     * Encodes all bytes in a SINGLE ANS stream, switching the probability
     * table per byte offset via netc_ctx_bucket(i). This gives per-position
     * entropy specialization (like MREG) with ZERO descriptor overhead.
     * Wire format: [state_sz initial_state][bitstream].
     *
     * Always try PCTX for multi-bucket packets. For small packets (<512B),
     * also try single-region best-fit and keep the smaller result. */
    if (dst_payload_cap >= state_sz) {
        netc_bsw_t bsw;
        netc_bsw_init(&bsw, dst + state_sz, dst_payload_cap - state_sz);

        uint32_t pctx_state = netc_tans_encode_pctx(
            dict->tables, src, src_size, &bsw, NETC_TANS_TABLE_SIZE);

        if (pctx_state != 0) {
            size_t pctx_bs = netc_bsw_flush(&bsw);
            if (pctx_bs != (size_t)-1) {
                size_t pctx_total = state_sz + pctx_bs;

                /* For small packets, also try single-region best-fit to compare.
                 * Skipped when NETC_INTERNAL_SKIP_SR is set (pre-filtered data:
                 * delta residuals or LZP XOR output) — PCTX with per-position
                 * tables dominates any single-region table for these inputs. */
                if (src_size <= 512u && !(ctx_flags & NETC_INTERNAL_SKIP_SR)) {
                    uint8_t trial_buf[520];
                    size_t  sr_cp = 0;
                    int     sr_mreg = 0, sr_x2 = 0;
                    uint32_t sr_tbl_idx = 0;

                    if (try_tans_single_region(dict, src, src_size,
                            trial_buf, sizeof(trial_buf),
                            &sr_cp, &sr_mreg, &sr_x2, &sr_tbl_idx,
                            ctx_flags, compact) == 0 && sr_cp < pctx_total)
                    {
                        /* Single-region beats PCTX — copy to dst and use it */
                        memcpy(dst, trial_buf, sr_cp);
                        *compressed_payload_size = sr_cp;
                        *used_mreg_flag  = sr_mreg;
                        *used_x2_flag    = sr_x2;
                        *out_table_idx   = sr_tbl_idx;
                        return 0;
                    }
                }

                /* PCTX wins — write initial state */
                if (compact)
                    netc_write_u16_le(dst, (uint16_t)pctx_state);
                else
                    netc_write_u32_le(dst, pctx_state);
                *compressed_payload_size = pctx_total;
                *used_mreg_flag = 2; /* signal PCTX to caller */
                *used_x2_flag   = 0;
                *out_table_idx  = 0;
                return 0;
            }
        }
    }

    /* Fallback: single-region best-fit table selection */
    return try_tans_single_region(dict, src, src_size, dst, dst_payload_cap,
                                  compressed_payload_size, used_mreg_flag,
                                  used_x2_flag, out_table_idx, ctx_flags,
                                  compact);
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
    if (NETC_UNLIKELY(dst_cap < NETC_COMPACT_HDR_MIN)) {
        return NETC_ERR_BUF_SMALL;
    }

    uint8_t seq  = ctx->context_seq++;
    const netc_dict_t *dict = ctx->dict;
    const int compact_mode = (ctx->flags & NETC_CFG_FLAG_COMPACT_HDR) ? 1 : 0;
    const size_t hdr_sz = compact_mode
        ? (src_size <= 127u ? NETC_COMPACT_HDR_MIN : NETC_COMPACT_HDR_MAX)
        : NETC_HEADER_SIZE;

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
    int            did_lzp      = 0;

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

    /* LZP XOR pre-filter: when the dictionary has a trained LZP table and
     * delta was NOT applied, XOR each byte with its position-aware LZP
     * prediction.  Correctly-predicted bytes become 0x00, concentrating the
     * distribution for much better tANS compression.  The tANS tables in
     * the dictionary were retrained on LZP-filtered data during training. */
    if (!did_delta && dict != NULL && dict->lzp_table != NULL &&
        ctx->arena_size >= src_size)
    {
        netc_lzp_xor_filter((const uint8_t *)src, src_size,
                            dict->lzp_table, ctx->arena);
        compress_src = ctx->arena;
        did_lzp      = 1;
    }

    /* Attempt tANS first if we have a valid dictionary */
    if (dict != NULL && src_size > 0) {
        size_t payload_cap = dst_cap - hdr_sz;
        uint8_t *payload   = (uint8_t *)dst + hdr_sz;
        size_t  compressed_payload = 0;
        int     used_mreg = 0;
        int     used_x2   = 0;
        uint32_t tbl_idx  = 0;

        /* When LZP is active in compact mode, suppress X2 (no compact
         * type for LZP+X2).  BIGRAM is supported via 0x90-0xAF types. */
        uint32_t tans_ctx_flags = ctx->flags;
        if (did_lzp && compact_mode)
            tans_ctx_flags |= NETC_INTERNAL_NO_X2;
        /* Skip single-region comparison for pre-filtered data (delta residuals
         * or LZP XOR output).  Both have position-specific distributions where
         * PCTX dominates any single-region table.  Saves 4-10 trial encodes.
         * FAST_COMPRESS extends this to all paths — PCTX always wins. */
        if (did_delta || did_lzp || (ctx->flags & NETC_CFG_FLAG_FAST_COMPRESS))
            tans_ctx_flags |= NETC_INTERNAL_SKIP_SR;

        if (try_tans_compress(dict, compress_src, src_size,
                              payload, payload_cap,
                              &compressed_payload, &used_mreg, &used_x2, &tbl_idx,
                              tans_ctx_flags, compact_mode) == 0 &&
            compressed_payload < src_size) {

            /* Delta-vs-LZP comparison: when delta+tANS succeeded but an LZP
             * table is available, also try LZP-only on raw bytes.  For small
             * packets (WL-001 64B, WL-002 128B) LZP often beats delta because
             * position-aware predictions are more accurate than inter-packet
             * deltas for structured fields.  Use stack buffers (bounded ≤512B).
             *
             * Strip BIGRAM from the trial flags: compact packet types for LZP
             * don't encode the BIGRAM flag (only LZP and LZP+DELTA are defined).
             * Using unigram tables in the trial matches what the decompressor
             * will use after reading the compact header.
             *
             * Adaptive skip: for large packets (>256B) where delta already
             * achieves ratio < 0.5 (compressed_payload < src_size/2), LZP is
             * unlikely to win — skip to save 1 LZP filter + 1 PCTX encode.
             * For ≤256B packets LZP is kept unconditional (predictions accurate
             * at short range; WL-001/002/003 depend on this for ratio). */
            if (did_delta && dict->lzp_table != NULL && src_size <= 512 &&
                !(ctx->flags & NETC_CFG_FLAG_FAST_COMPRESS) &&
                (src_size <= 256u || compressed_payload >= (src_size >> 1))) {
                uint8_t lzp_trial_src[512];
                uint8_t lzp_trial_dst[520];
                netc_lzp_xor_filter((const uint8_t *)src, src_size,
                                    dict->lzp_table, lzp_trial_src);

                size_t  lzp_cp = 0;
                int     lzp_mreg = 0, lzp_x2 = 0;
                uint32_t lzp_tbl = 0;
                /* Strip DELTA, suppress X2.  BIGRAM is now supported
                 * via LZP+BIGRAM compact types (0x90-0xAF).
                 * Also skip SR comparison — LZP-filtered data is optimally
                 * encoded by PCTX (per-position tables trained on LZP output). */
                uint32_t lzp_ctx = (ctx->flags
                    & ~(uint32_t)NETC_CFG_FLAG_DELTA)
                    | (compact_mode ? NETC_INTERNAL_NO_X2 : 0u)
                    | NETC_INTERNAL_SKIP_SR;
                if (try_tans_compress(dict, lzp_trial_src, src_size,
                                      lzp_trial_dst, sizeof(lzp_trial_dst),
                                      &lzp_cp, &lzp_mreg, &lzp_x2, &lzp_tbl,
                                      lzp_ctx, compact_mode) == 0 &&
                    lzp_cp < compressed_payload)
                {
                    /* LZP-only beats delta — switch to LZP result */
                    memcpy(payload, lzp_trial_dst, lzp_cp);
                    compressed_payload = lzp_cp;
                    used_mreg = lzp_mreg;
                    used_x2   = lzp_x2;
                    tbl_idx   = lzp_tbl;
                    pkt_flags &= ~(uint8_t)NETC_PKT_FLAG_DELTA;
                    did_delta  = 0;
                    did_lzp    = 1;
                    tans_ctx_flags = lzp_ctx; /* match the trial encoding flags */
                }
            }

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
            /* Skip LZ77 for small packets (<256B, or <512B in FAST_COMPRESS mode):
             * hash table init (8KB) + scan overhead exceeds the ratio gain.
             * FAST_COMPRESS raises the threshold to 512B for extra speed. */
            {
            size_t lz77_min = (ctx->flags & NETC_CFG_FLAG_FAST_COMPRESS)
                              ? 512u : 256u;
            if (src_size >= lz77_min && compressed_payload * 2 > src_size) {
                if (!did_delta && ctx->arena_size >= src_size) {
                    /* Case A: LZ77 into arena, tANS stays in dst payload.
                     * Always use raw src for LZ77 (not LZP-filtered data)
                     * since LZ77 packets don't carry LZP inverse info. */
                    size_t lz_len = lz77_encode((const uint8_t *)src, src_size,
                                                ctx->arena, ctx->arena_size);
                    if (lz_len < compressed_payload && lz_len < src_size &&
                        hdr_sz + lz_len <= dst_cap) {
                        /* LZ77 wins: copy from arena to dst payload */
                        memcpy((uint8_t *)dst + hdr_sz, ctx->arena, lz_len);
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lz_len;
                        hdr.flags           = pkt_flags | NETC_PKT_FLAG_LZ77
                                              | NETC_PKT_FLAG_PASSTHRU;
                        hdr.algorithm       = NETC_ALG_PASSTHRU;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_emit(dst, &hdr, compact_mode);
                        *dst_size = hdr_sz + lz_len;
                        ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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
                    int      tans_used_mreg_save = used_mreg;
                    int      tans_used_x2_save   = used_x2;
                    uint32_t tans_tbl_idx_save   = tbl_idx;
                    size_t   tans_cp_save        = compressed_payload;

                    size_t lz_len = lz77_encode(compress_src, src_size,
                                                payload, payload_cap);
                    if (lz_len < tans_cp_save && lz_len < src_size &&
                        hdr_sz + lz_len <= dst_cap) {
                        /* LZ77 wins */
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lz_len;
                        hdr.flags           = pkt_flags | NETC_PKT_FLAG_LZ77
                                              | NETC_PKT_FLAG_PASSTHRU;
                        hdr.algorithm       = NETC_ALG_PASSTHRU;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_emit(dst, &hdr, compact_mode);
                        *dst_size = hdr_sz + lz_len;
                        ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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
                    tbl_idx   = tans_tbl_idx_save;
                    goto case_b_skip;
                }
case_b_skip:;
            }
            } /* end lz77_min block */

            /* Cross-packet LZ77 competition: try LZ77X when any of:
             * (a) tANS ratio is poor (> 0.5), or
             * (b) current packet is highly similar to previous packet
             *     (fast 32-byte prefix check, ≥50% match), or
             * (c) data has low symbol diversity (≤4 distinct values in first
             *     32 bytes) — indicates repetitive/patterned data that likely
             *     has ring-buffer matches from earlier packets, even if the
             *     immediately preceding packet differs (e.g. cycling patterns).
             * Gate on ≥64B to avoid overhead on tiny packets.
             * Encode raw src (not residuals) to use ring-buffer back-refs. */
            if (ctx->ring != NULL && ctx->ring_size > 0 &&
                ctx->prev_pkt_size > 0 &&
                src_size >= 64u &&
                ctx->arena_size >= src_size)
            {
                /* Fast pre-check: skip expensive LZ77X if data is unlikely
                 * to have cross-packet matches. */
                int try_lzx = (compressed_payload * 2 > src_size); /* ratio > 0.5 */
                if (!try_lzx && ctx->prev_pkt != NULL &&
                    ctx->prev_pkt_size == src_size)
                {
                    uint32_t check = (uint32_t)src_size;
                    if (check > 32) check = 32;
                    uint32_t matches = 0;
                    for (uint32_t q = 0; q < check; q++) {
                        if (((const uint8_t *)src)[q] == ctx->prev_pkt[q])
                            matches++;
                    }
                    if (matches * 2 >= check) try_lzx = 1; /* ≥50% similar */
                }
                /* (c) Low symbol diversity: ≤4 distinct values → repetitive */
                if (!try_lzx) {
                    const uint8_t *raw = (const uint8_t *)src;
                    uint32_t scan = (uint32_t)src_size;
                    if (scan > 32) scan = 32;
                    uint8_t seen[4]; unsigned ns = 0; int diverse = 0;
                    for (uint32_t q = 0; q < scan; q++) {
                        uint8_t b = raw[q]; unsigned found = 0;
                        for (unsigned u = 0; u < ns; u++)
                            if (seen[u] == b) { found = 1; break; }
                        if (!found) {
                            if (ns >= 4) { diverse = 1; break; }
                            seen[ns++] = b;
                        }
                    }
                    if (!diverse) try_lzx = 1;
                }

                if (try_lzx) {
                    size_t lzx_len = lz77x_encode(
                        (const uint8_t *)src, src_size,
                        ctx->ring, ctx->ring_size, ctx->ring_pos,
                        (uint32_t)ctx->prev_pkt_size,
                        ctx->arena, ctx->arena_size);
                    if (lzx_len != (size_t)-1 && lzx_len < compressed_payload &&
                        lzx_len < src_size &&
                        hdr_sz + lzx_len <= dst_cap)
                    {
                        /* Cross-packet LZ77 wins */
                        memcpy((uint8_t *)dst + hdr_sz, ctx->arena, lzx_len);
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lzx_len;
                        hdr.flags           = NETC_PKT_FLAG_DICT_ID;
                        hdr.algorithm       = NETC_ALG_LZ77X;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_emit(dst, &hdr, compact_mode);
                        *dst_size = hdr_sz + lzx_len;
                        ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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
            }

            /* --- 10-bit tANS competition for small packets (<=128B) ---
             *
             * For small packets, the 12-bit tANS table may waste bits on
             * infrequent symbols (each gets at least 1/4096 of the state space).
             * A 10-bit table (1024 entries) can be more efficient because:
             *   (a) Smaller table = less per-symbol overhead for rare symbols
             *   (b) Better L1 cache utilization (7.5 KB vs 28 KB)
             *   (c) State fits in 11 bits [1024,2048) — still fits uint16
             *
             * Only try 10-bit when: single-region (used_mreg==0), no MREG/PCTX,
             * no LZP, no bigram, compact mode, and <=128B.
             * The 10-bit table is built on-the-fly from the winning 12-bit table. */
            int used_tans_10 = 0;
            if (src_size <= 128u &&
                used_mreg == 0 && !did_lzp &&
                !(tans_ctx_flags & NETC_CFG_FLAG_BIGRAM) &&
                compact_mode)
            {
                /* Rescale the winning 12-bit freq table to 10-bit (1024-sum) */
                const netc_tans_table_t *tbl12 = &dict->tables[tbl_idx];
                netc_freq_table_t freq10;
                if (netc_freq_rescale_12_to_10(&tbl12->freq, &freq10) == 0) {
                    netc_tans_table_10_t tbl10;
                    if (netc_tans_build_10(&tbl10, &freq10) == 0) {
                        uint8_t trial10[136]; /* 128B + 8B overhead max */
                        size_t cp10 = try_tans_10bit_with_table(
                            &tbl10, compress_src, src_size,
                            trial10, sizeof(trial10));
                        if (cp10 != (size_t)-1 && cp10 < compressed_payload) {
                            /* 10-bit wins — copy to dst payload */
                            memcpy(payload, trial10, cp10);
                            compressed_payload = cp10;
                            used_tans_10 = 1;
                            used_x2 = 0; /* 10-bit never uses x2 */
                        }
                    }
                }
            }

            /* tANS wins.  used_mreg: 0=single-region, 1=MREG, 2=PCTX.
             * Single-region: encode table index in upper 4 bits of algorithm byte.
             * MREG: algorithm=NETC_ALG_TANS, flags|=MREG.
             * PCTX: algorithm=NETC_ALG_TANS_PCTX (per-position context).
             * 10-bit: algorithm=NETC_ALG_TANS_10 (adaptive small-packet). */
            uint8_t extra_flags = (used_mreg == 1 ? NETC_PKT_FLAG_MREG : 0)
                                | (used_x2   ? NETC_PKT_FLAG_X2     : 0)
                                | ((tans_ctx_flags & NETC_CFG_FLAG_BIGRAM) ? NETC_PKT_FLAG_BIGRAM : 0);
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)compressed_payload;
            hdr.flags           = pkt_flags | extra_flags;
            if (used_tans_10) {
                hdr.algorithm = (uint8_t)(NETC_ALG_TANS_10 | (tbl_idx << 4));
            } else if (did_lzp) {
                /* LZP XOR pre-filter was applied; signal decompressor to invert.
                 * For PCTX: use NETC_ALG_TANS_PCTX | 0x10 (upper nibble marks LZP).
                 * For single-region/MREG: algorithm = NETC_ALG_LZP. */
                if (used_mreg == 2) {
                    hdr.algorithm = NETC_ALG_TANS_PCTX | 0x10u;
                } else {
                    hdr.algorithm = (uint8_t)(NETC_ALG_LZP | ((used_mreg ? 0u : tbl_idx) << 4));
                }
            } else if (used_mreg == 2) {
                hdr.algorithm = NETC_ALG_TANS_PCTX;
            } else {
                hdr.algorithm = (uint8_t)(NETC_ALG_TANS | ((used_mreg ? 0u : tbl_idx) << 4));
            }
            hdr.model_id        = dict->model_id;
            hdr.context_seq     = seq;
            netc_hdr_emit(dst, &hdr, compact_mode);
            *dst_size = hdr_sz + compressed_payload;
            ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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

    /* --- tANS-raw fallback: if delta was applied but tANS on residuals failed,
     * try tANS on the original (raw) bytes instead.  This helps when the delta
     * residuals have higher entropy than the raw bytes (e.g. random sequences). */
    if (did_delta && dict != NULL && src_size > 0) {
        size_t payload_cap = dst_cap - hdr_sz;
        uint8_t *payload   = (uint8_t *)dst + hdr_sz;
        size_t  raw_payload = 0;
        int     raw_mreg   = 0;
        int     raw_x2     = 0;
        uint32_t raw_tbl   = 0;

        uint32_t raw_ctx_flags = ctx->flags & ~(uint32_t)NETC_CFG_FLAG_DELTA;
        /* When LZP table is available, apply XOR pre-filter to raw bytes
         * before re-trying tANS (reuses arena since delta residuals are
         * no longer needed in this fallback path). */
        const uint8_t *raw_src = (const uint8_t *)src;
        int fallback_lzp = 0;
        if (dict->lzp_table != NULL && ctx->arena_size >= src_size) {
            netc_lzp_xor_filter((const uint8_t *)src, src_size,
                                dict->lzp_table, ctx->arena);
            raw_src = ctx->arena;
            fallback_lzp = 1;
            /* Suppress X2 for LZP compact (no LZP+X2 type).
             * BIGRAM is supported via 0x90-0xAF compact types. */
            if (compact_mode)
                raw_ctx_flags |= NETC_INTERNAL_NO_X2;
        }
        if (try_tans_compress(dict, raw_src, src_size,
                              payload, payload_cap,
                              &raw_payload, &raw_mreg, &raw_x2, &raw_tbl,
                              raw_ctx_flags, compact_mode) == 0 &&
            raw_payload < src_size)
        {
            /* Cross-packet LZ77X competition on the raw-tANS fallback path.
             * Arena held delta residuals but those are no longer needed — we
             * can reuse it as LZ77X output buffer. */
            if (ctx->ring != NULL && ctx->ring_size > 0 &&
                ctx->prev_pkt_size > 0 && src_size >= 64u &&
                ctx->arena_size >= src_size)
            {
                int try_lzx = (raw_payload * 2 > src_size);
                if (!try_lzx && ctx->prev_pkt != NULL &&
                    ctx->prev_pkt_size == src_size)
                {
                    uint32_t check = (uint32_t)src_size;
                    if (check > 32) check = 32;
                    uint32_t matches = 0;
                    for (uint32_t q = 0; q < check; q++) {
                        if (((const uint8_t *)src)[q] == ctx->prev_pkt[q])
                            matches++;
                    }
                    if (matches * 2 >= check) try_lzx = 1;
                }
                if (!try_lzx) {
                    const uint8_t *raw = (const uint8_t *)src;
                    uint32_t scan = (uint32_t)src_size;
                    if (scan > 32) scan = 32;
                    uint8_t seen[4]; unsigned ns = 0; int diverse = 0;
                    for (uint32_t q = 0; q < scan; q++) {
                        uint8_t b = raw[q]; unsigned found = 0;
                        for (unsigned u = 0; u < ns; u++)
                            if (seen[u] == b) { found = 1; break; }
                        if (!found) {
                            if (ns >= 4) { diverse = 1; break; }
                            seen[ns++] = b;
                        }
                    }
                    if (!diverse) try_lzx = 1;
                }
                if (try_lzx) {
                    size_t lzx_len = lz77x_encode(
                        (const uint8_t *)src, src_size,
                        ctx->ring, ctx->ring_size, ctx->ring_pos,
                        (uint32_t)ctx->prev_pkt_size,
                        ctx->arena, ctx->arena_size);
                    if (lzx_len != (size_t)-1 && lzx_len < raw_payload &&
                        lzx_len < src_size &&
                        hdr_sz + lzx_len <= dst_cap)
                    {
                        /* LZ77X beats raw tANS — emit */
                        memcpy(payload, ctx->arena, lzx_len);
                        netc_pkt_header_t hdr;
                        hdr.original_size   = (uint16_t)src_size;
                        hdr.compressed_size = (uint16_t)lzx_len;
                        hdr.flags           = NETC_PKT_FLAG_DICT_ID;
                        hdr.algorithm       = NETC_ALG_LZ77X;
                        hdr.model_id        = dict->model_id;
                        hdr.context_seq     = seq;
                        netc_hdr_emit(dst, &hdr, compact_mode);
                        *dst_size = hdr_sz + lzx_len;
                        ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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
            }

            /* --- 10-bit tANS competition for raw-tANS fallback (small packets) --- */
            int raw_tans_10 = 0;
            if (src_size <= 128u &&
                raw_mreg == 0 && !fallback_lzp &&
                !(raw_ctx_flags & NETC_CFG_FLAG_BIGRAM) &&
                compact_mode)
            {
                const netc_tans_table_t *tbl12 = &dict->tables[raw_tbl];
                netc_freq_table_t freq10;
                if (netc_freq_rescale_12_to_10(&tbl12->freq, &freq10) == 0) {
                    netc_tans_table_10_t tbl10;
                    if (netc_tans_build_10(&tbl10, &freq10) == 0) {
                        uint8_t trial10[136];
                        size_t cp10 = try_tans_10bit_with_table(
                            &tbl10, raw_src, src_size,
                            trial10, sizeof(trial10));
                        if (cp10 != (size_t)-1 && cp10 < raw_payload) {
                            memcpy(payload, trial10, cp10);
                            raw_payload = cp10;
                            raw_tans_10 = 1;
                            raw_x2 = 0;
                        }
                    }
                }
            }

            /* Raw tANS succeeds — emit without the delta flag but preserve bigram */
            uint8_t extra_flags = (raw_mreg == 1 ? NETC_PKT_FLAG_MREG : 0)
                                | (raw_x2   ? NETC_PKT_FLAG_X2   : 0)
                                | ((raw_ctx_flags & NETC_CFG_FLAG_BIGRAM) ? NETC_PKT_FLAG_BIGRAM : 0);
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)raw_payload;
            hdr.flags           = NETC_PKT_FLAG_DICT_ID | extra_flags;
            if (raw_tans_10) {
                hdr.algorithm = (uint8_t)(NETC_ALG_TANS_10 | (raw_tbl << 4));
            } else if (fallback_lzp) {
                if (raw_mreg == 2) {
                    hdr.algorithm = NETC_ALG_TANS_PCTX | 0x10u;
                } else {
                    hdr.algorithm = (uint8_t)(NETC_ALG_LZP | ((raw_mreg ? 0u : raw_tbl) << 4));
                }
            } else if (raw_mreg == 2) {
                hdr.algorithm = NETC_ALG_TANS_PCTX;
            } else {
                hdr.algorithm = (uint8_t)(NETC_ALG_TANS | ((raw_mreg ? 0u : raw_tbl) << 4));
            }
            hdr.model_id        = dict->model_id;
            hdr.context_seq     = seq;
            netc_hdr_emit(dst, &hdr, compact_mode);
            *dst_size = hdr_sz + raw_payload;
            ctx_ring_append(ctx, (const uint8_t *)src, src_size);
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

    /* --- Arena corruption guard ---
     * If the raw-tANS fallback block above was entered (did_delta && dict has
     * LZP table), the arena was overwritten with LZP-filtered raw bytes by
     * netc_lzp_xor_filter.  compress_src still points to the arena but its
     * content is no longer delta residuals.  Reset to use raw src bytes and
     * clear the DELTA flag so LZ77/passthrough don't assume delta encoding. */
    if (did_delta && dict != NULL && dict->lzp_table != NULL &&
        ctx->arena_size >= src_size)
    {
        compress_src = (const uint8_t *)src;
        pkt_flags   &= ~(uint8_t)NETC_PKT_FLAG_DELTA;
        did_delta    = 0;
    }

    /* --- LZ77 path: tANS failed/unavailable or didn't compress.
     * When a ring buffer is available (stateful mode), use cross-packet LZ77
     * (NETC_ALG_LZ77X) which searches the ring buffer history for matches.
     * Otherwise fall back to within-packet LZ77 (NETC_ALG_PASSTHRU + LZ77). */
    if (src_size > 0 && dst_cap > hdr_sz) {
        uint8_t *out_payload = (uint8_t *)dst + hdr_sz;
        size_t   out_cap     = dst_cap - hdr_sz;
        size_t   lz_len      = (size_t)-1;
        uint8_t  lz_alg      = NETC_ALG_PASSTHRU;

        /* Within-packet LZ77 (always tried first).
         * Use raw bytes (not LZP-filtered) since LZ77 packets don't carry
         * LZP inverse info.  When did_delta, compress_src is delta residuals
         * and the DELTA flag propagates to the LZ77 packet (correct). */
        const uint8_t *lz77_src = did_lzp ? (const uint8_t *)src : compress_src;
        lz_len = lz77_encode(lz77_src, src_size, out_payload, out_cap);
        if (lz_len != (size_t)-1 && lz_len < src_size) {
            lz_alg = NETC_ALG_PASSTHRU;
        } else {
            lz_len = (size_t)-1;
        }

        /* Cross-packet LZ77 (tried when ring buffer has primed history and
         * within-packet LZ77 failed or didn't compress enough).
         * Gate on prev_pkt_size > 0 and src_size >= 64: avoid trying LZ77X
         * on tiny packets where 3-byte long back-ref tokens can't save enough.
         * IMPORTANT: always encode raw src bytes (never delta residuals) so
         * the decoder does not need to apply an inverse delta pass. */
        if (lz_len == (size_t)-1 &&
            src_size >= 64u &&
            ctx->ring != NULL && ctx->ring_size > 0 &&
            ctx->prev_pkt_size > 0)
        {
            size_t lz_x = lz77x_encode((const uint8_t *)src, src_size,
                                        ctx->ring, ctx->ring_size, ctx->ring_pos,
                                        (uint32_t)ctx->prev_pkt_size,
                                        out_payload, out_cap);
            if (lz_x != (size_t)-1 && lz_x < src_size) {
                lz_len = lz_x;
                lz_alg = NETC_ALG_LZ77X;
            }
        }

        if (lz_len == (size_t)-1) goto lz77_failed;

        {
            /* For cross-packet LZ77, emit without PASSTHRU+LZ77 flags (new alg byte).
             * For within-packet LZ77, keep legacy PASSTHRU+LZ77 flags. */
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)lz_len;
            if (lz_alg == NETC_ALG_LZ77X) {
                /* Cross-packet LZ77: no DELTA flag (always encodes raw src),
                 * DICT_ID for model tracking. No PASSTHRU/LZ77 flags. */
                hdr.flags     = NETC_PKT_FLAG_DICT_ID;
                hdr.algorithm = NETC_ALG_LZ77X;
                hdr.model_id  = (dict != NULL) ? dict->model_id : 0;
            } else {
                hdr.flags     = pkt_flags | NETC_PKT_FLAG_LZ77 | NETC_PKT_FLAG_PASSTHRU;
                hdr.algorithm = NETC_ALG_PASSTHRU;
                hdr.model_id  = (dict != NULL) ? dict->model_id : 0;
            }
            hdr.context_seq = seq;
            netc_hdr_emit(dst, &hdr, compact_mode);
            *dst_size = hdr_sz + lz_len;

            /* Append original bytes to ring buffer BEFORE updating prev_pkt,
             * since we always encode raw src (not residuals) for LZ77X. */
            ctx_ring_append(ctx, (const uint8_t *)src, src_size);

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
    /* Ring buffer update even for passthrough — decoder always has original bytes */
    ctx_ring_append(ctx, (const uint8_t *)src, src_size);

    /* Fall back to passthrough (no delta flag — raw bytes in payload) */
    return emit_passthrough(ctx, dict, src, src_size, dst, dst_cap, dst_size, seq,
                            compact_mode);
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
        uint32_t tbl_idx   = 0;

        /* LZP XOR pre-filter for stateless path (stack buffer, ≤1024B) */
        const uint8_t *tans_src = (const uint8_t *)src;
        int sl_did_lzp = 0;
        uint8_t lzp_filt_buf[1024];
        if (dict != NULL && dict->lzp_table != NULL && src_size <= 1024) {
            netc_lzp_xor_filter((const uint8_t *)src, src_size,
                                dict->lzp_table, lzp_filt_buf);
            tans_src = lzp_filt_buf;
            sl_did_lzp = 1;
        }

        int tans_ok = (try_tans_compress(dict, tans_src, src_size,
                                         payload, payload_cap,
                                         &compressed_payload, &used_mreg, &used_x2,
                                         &tbl_idx,
                                         0U /* stateless: no bigram */,
                                         0  /* stateless: no compact state */) == 0
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
            uint8_t extra_flags = (used_mreg == 1 ? NETC_PKT_FLAG_MREG : 0)
                                | (used_x2   ? NETC_PKT_FLAG_X2   : 0);
            netc_pkt_header_t hdr;
            hdr.original_size   = (uint16_t)src_size;
            hdr.compressed_size = (uint16_t)compressed_payload;
            hdr.flags           = NETC_PKT_FLAG_DICT_ID | extra_flags;
            if (sl_did_lzp) {
                if (used_mreg == 2) {
                    hdr.algorithm = NETC_ALG_TANS_PCTX | 0x10u;
                } else {
                    hdr.algorithm = (uint8_t)(NETC_ALG_LZP | ((used_mreg ? 0u : tbl_idx) << 4));
                }
            } else if (used_mreg == 2) {
                hdr.algorithm = NETC_ALG_TANS_PCTX;
            } else {
                hdr.algorithm = (uint8_t)(NETC_ALG_TANS | ((used_mreg ? 0u : tbl_idx) << 4));
            }
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

    return emit_passthrough(NULL, dict, src, src_size, dst, dst_cap, dst_size, 0,
                            0 /* legacy header for stateless */);
}
