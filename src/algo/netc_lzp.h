/**
 * netc_lzp.h — LZP (Lempel-Ziv Prediction) codec internal types and interface.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * LZP uses position-aware order-1 context prediction (matching OodleNetwork):
 *   Context = hash(previous_byte, byte_offset_in_packet)
 *   ORDER   = 1  (1 byte of context + position → predict next byte)
 *   HT_BITS = 17 (131072 hash table entries, matches Oodle htbits=17)
 *
 * The LZP codec predicts each byte by hashing the previous byte together
 * with the byte's position in the packet.  Position-awareness means the
 * model learns per-offset byte distributions, which is critical for
 * structured network packets where byte semantics depend on position.
 *
 * Wire format (NETC_ALG_LZP payload):
 *   [2B]  n_literals          (uint16 LE)
 *   [FB]  flag_bits           (packed bitstream, MSB-first, FB = ceil(src_size/8))
 *   [NL]  literal_bytes       (NL = n_literals, raw unpredicted bytes in order)
 *
 * Total payload = 2 + ceil(src_size/8) + n_literals.
 * LZP is only emitted when payload < src_size (otherwise tANS/passthrough wins).
 */

#ifndef NETC_LZP_H
#define NETC_LZP_H

#include "../util/netc_platform.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * LZP parameters
 * ========================================================================= */

#define NETC_LZP_ORDER      1U
#define NETC_LZP_HT_BITS   17U
#define NETC_LZP_HT_SIZE   (1U << NETC_LZP_HT_BITS)   /* 131072 */
#define NETC_LZP_HT_MASK   (NETC_LZP_HT_SIZE - 1U)

/* =========================================================================
 * LZP hash table entry — trained prediction
 * ========================================================================= */

typedef struct {
    uint8_t value;  /* Predicted byte for this (prev_byte, position) context */
    uint8_t valid;  /* 1 = prediction is trained, 0 = empty slot */
} netc_lzp_entry_t;

/* =========================================================================
 * LZP hash function — position-aware order-1 context
 *
 * Hashes (previous_byte, byte_position) to select a hash table slot.
 * Position-awareness is critical: byte 5 in a game state packet has
 * completely different semantics than byte 50, even if the previous
 * byte happens to be the same.
 *
 * For position 0 (first byte), prev_byte is the implicit 0x00 start byte.
 * ========================================================================= */

static NETC_INLINE uint32_t netc_lzp_hash(uint8_t prev_byte, uint32_t pos)
{
    uint32_t h = 2166136261u;
    h ^= prev_byte;        h *= 16777619u;
    h ^= (pos & 0xFFFFu);  h *= 16777619u;
    h ^= (pos >> 16);      h *= 16777619u;
    return h & NETC_LZP_HT_MASK;
}

/* Keep netc_lzp_hash3 as a backward-compatible alias for training code
 * that uses 3-byte context without position (unused in current code but
 * preserves the interface). */
static NETC_INLINE uint32_t netc_lzp_hash3(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    h ^= p[0]; h *= 16777619u;
    h ^= p[1]; h *= 16777619u;
    h ^= p[2]; h *= 16777619u;
    return h & NETC_LZP_HT_MASK;
}

/* =========================================================================
 * LZP predict (compress side)
 *
 * For each byte, hashes (prev_byte, position) and looks up the prediction.
 * Emits a packed flag bitstream + literal bytes for misses.
 *
 * dst layout:
 *   [2B] n_literals (uint16 LE)
 *   [flag_bytes] packed bits (1=match, 0=miss), MSB-first
 *   [n_literals] literal bytes
 *
 * Returns total bytes written to dst, or (size_t)-1 if LZP output >= src_size.
 * ========================================================================= */

static NETC_INLINE size_t netc_lzp_predict(
    const uint8_t          *src,
    size_t                  src_size,
    const netc_lzp_entry_t *lzp_table,
    uint8_t                *dst,
    size_t                  dst_cap)
{
    if (src_size < 2) return (size_t)-1;  /* too small for any benefit */

    size_t flag_bytes = (src_size + 7u) / 8u;
    /* worst case: all misses → 2 + flag_bytes + src_size */
    if (dst_cap < 2u + flag_bytes + src_size) return (size_t)-1;

    /* flags go to dst+2, literals go after flags */
    uint8_t *flag_dst = dst + 2;
    memset(flag_dst, 0, flag_bytes);

    uint8_t *lit_dst = flag_dst + flag_bytes;
    uint16_t n_literals = 0;

    for (size_t i = 0; i < src_size; i++) {
        /* Context: previous byte (0x00 for first byte) + position */
        uint8_t prev = (i > 0) ? src[i - 1] : 0x00u;
        uint32_t h = netc_lzp_hash(prev, (uint32_t)i);

        if (lzp_table[h].valid && lzp_table[h].value == src[i]) {
            /* Match — set flag bit to 1 */
            flag_dst[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
        } else {
            /* Miss — flag bit stays 0, emit literal */
            lit_dst[n_literals++] = src[i];
        }
    }

    /* Write n_literals header */
    dst[0] = (uint8_t)(n_literals & 0xFFu);
    dst[1] = (uint8_t)(n_literals >> 8);

    size_t total = 2u + flag_bytes + (size_t)n_literals;
    return (total < src_size) ? total : (size_t)-1;
}

/* =========================================================================
 * LZP reconstruct (decompress side)
 *
 * Reverses netc_lzp_predict: reads flag bitstream and literals to
 * reconstruct the original bytes using position-aware order-1 context.
 *
 * src layout:
 *   [2B] n_literals (uint16 LE)
 *   [flag_bytes] packed bits (1=match, 0=miss), MSB-first
 *   [n_literals] literal bytes
 *
 * Returns 0 on success, -1 on corrupt input.
 * ========================================================================= */

static NETC_INLINE int netc_lzp_reconstruct(
    const uint8_t          *src,
    size_t                  src_size,
    const netc_lzp_entry_t *lzp_table,
    uint8_t                *dst,
    size_t                  dst_size)
{
    if (dst_size < 2) return -1;
    if (src_size < 2) return -1;

    uint16_t n_literals = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
    size_t flag_bytes = (dst_size + 7u) / 8u;

    if (src_size < 2u + flag_bytes + (size_t)n_literals) return -1;

    const uint8_t *flag_data = src + 2;
    const uint8_t *lit_data  = flag_data + flag_bytes;
    uint16_t lit_idx = 0;

    for (size_t i = 0; i < dst_size; i++) {
        int flag = (flag_data[i >> 3] >> (7u - (i & 7u))) & 1;

        /* Context: previous decoded byte (0x00 for first byte) + position */
        uint8_t prev = (i > 0) ? dst[i - 1] : 0x00u;
        uint32_t h = netc_lzp_hash(prev, (uint32_t)i);

        if (flag) {
            /* Match — predict from hash table */
            if (!lzp_table[h].valid) return -1;  /* corrupt: flag=1 but no prediction */
            dst[i] = lzp_table[h].value;
        } else {
            /* Miss — read literal */
            if (lit_idx >= n_literals) return -1;
            dst[i] = lit_data[lit_idx++];
        }
    }

    return (lit_idx == n_literals) ? 0 : -1;
}

/* =========================================================================
 * LZP XOR pre-filter (compress side)
 *
 * XOR each byte with its LZP prediction. Correctly-predicted bytes become
 * 0x00, concentrating the distribution and reducing entropy for the
 * downstream tANS encoder. Bytes with no valid prediction pass through
 * unchanged (XOR with 0).
 *
 * This is a composable pre-filter — the output has the same size as
 * src_size and feeds directly into tANS multi-region encoding.
 *
 * prev_byte_out: if non-NULL, receives the last decoded (original) byte
 * for use in sequential contexts. Not used currently.
 * ========================================================================= */

static NETC_INLINE void netc_lzp_xor_filter(
    const uint8_t          *src,
    size_t                  src_size,
    const netc_lzp_entry_t *lzp_table,
    uint8_t                *dst)
{
    for (size_t i = 0; i < src_size; i++) {
        uint8_t prev = (i > 0) ? src[i - 1] : 0x00u;
        uint32_t h = netc_lzp_hash(prev, (uint32_t)i);
        uint8_t prediction = (lzp_table[h].valid) ? lzp_table[h].value : 0x00u;
        dst[i] = src[i] ^ prediction;
    }
}

/* =========================================================================
 * LZP XOR inverse filter (decompress side)
 *
 * Reverses the XOR pre-filter: XOR each byte with the same prediction to
 * recover the original data. Must be applied AFTER tANS decoding.
 *
 * IMPORTANT: This operates in-place or src→dst. Because the prediction
 * for byte i depends on the ORIGINAL byte i-1 (not the filtered byte),
 * we must reconstruct sequentially: decode byte 0, use it to predict
 * byte 1, etc.
 * ========================================================================= */

static NETC_INLINE void netc_lzp_xor_unfilter(
    const uint8_t          *src,
    size_t                  src_size,
    const netc_lzp_entry_t *lzp_table,
    uint8_t                *dst)
{
    for (size_t i = 0; i < src_size; i++) {
        /* Previous ORIGINAL byte (already reconstructed in dst) */
        uint8_t prev = (i > 0) ? dst[i - 1] : 0x00u;
        uint32_t h = netc_lzp_hash(prev, (uint32_t)i);
        uint8_t prediction = (lzp_table[h].valid) ? lzp_table[h].value : 0x00u;
        dst[i] = src[i] ^ prediction;
    }
}

/* =========================================================================
 * LZP adaptive update (miss-driven)
 *
 * After each packet, scans through the raw bytes and updates the mutable
 * LZP hash table on prediction misses.  When the table's prediction for
 * context (prev_byte, position) doesn't match the actual byte, overwrite
 * it.  This lets the LZP table learn actual per-position byte patterns
 * from the live connection, improving prediction hit rate over time.
 *
 * Called identically on both encoder and decoder with the same raw bytes,
 * keeping the tables in sync without any wire overhead.
 * ========================================================================= */

static NETC_INLINE void netc_lzp_adaptive_update(
    netc_lzp_entry_t *lzp_table,
    const uint8_t    *data,
    size_t            data_size)
{
    if (!lzp_table || !data || data_size == 0) return;

    for (size_t i = 0; i < data_size; i++) {
        uint8_t prev = (i > 0) ? data[i - 1] : 0x00u;
        uint32_t h = netc_lzp_hash(prev, (uint32_t)i);

        if (!lzp_table[h].valid) {
            /* Empty slot — fill with observed byte */
            lzp_table[h].value = data[i];
            lzp_table[h].valid = 1;
        } else if (lzp_table[h].value != data[i]) {
            /* Prediction miss on a trained slot.  Use a lightweight
             * exponential-decay replacement: the `valid` field doubles
             * as a confidence counter (1-255).  On miss, decrement;
             * when it reaches 0, overwrite with the new value.  On hit,
             * saturating-increment toward 255.  This prevents thrashing
             * from hash collisions while still adapting to distribution
             * shifts over many packets. */
            if (lzp_table[h].valid > 1) {
                lzp_table[h].valid--;
            } else {
                /* Confidence depleted — replace prediction */
                lzp_table[h].value = data[i];
                lzp_table[h].valid = 1;
            }
        } else {
            /* Hit — boost confidence (saturating increment) */
            if (lzp_table[h].valid < 255)
                lzp_table[h].valid++;
        }
    }
}

#endif /* NETC_LZP_H */
