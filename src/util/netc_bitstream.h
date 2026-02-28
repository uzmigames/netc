/**
 * netc_bitstream.h — Bit-level I/O for the tANS codec.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * Writer: LSB-first, 64-bit accumulator. Bits are packed from LSB to MSB
 * within each byte, flushed to the output buffer when the accumulator fills.
 * netc_bsw_flush() appends a sentinel 1-bit so the reader can locate the
 * exact starting position within the last byte.
 *
 * Reader: Reads bytes from the END of the buffer into a 64-bit accumulator
 * arranged so the LAST byte of the stream sits in bits [63..56] (the MSB).
 * Bits are consumed from the MSB downward (left-shift to discard).
 * On init the sentinel bit is located and consumed so subsequent reads
 * return only actual data bits in the correct order.
 *
 * Encoding direction: tANS encodes symbols in reverse (src[N-1] .. src[0])
 * and emits bits forward. The decoder therefore reads the bitstream backward
 * (from the last byte toward the first), recovering src[0] first.
 * The sentinel ensures byte-boundary alignment is handled transparently.
 */

#ifndef NETC_BITSTREAM_H
#define NETC_BITSTREAM_H

#include "netc_platform.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Bitstream writer
 * ========================================================================= */

typedef struct {
    uint8_t *start;    /* Start of output buffer */
    uint8_t *ptr;      /* Current write position */
    uint8_t *end;      /* One past last byte of buffer */
    uint64_t accum;    /* Bit accumulator (LSB = next bit to write) */
    int      bits;     /* Number of valid bits in accum (0–63) */
} netc_bsw_t;

/** Initialize a bitstream writer over [buf, buf+cap). */
static NETC_INLINE void netc_bsw_init(netc_bsw_t *w, void *buf, size_t cap) {
    w->start = (uint8_t *)buf;
    w->ptr   = (uint8_t *)buf;
    w->end   = (uint8_t *)buf + cap;
    w->accum = 0;
    w->bits  = 0;
}

/**
 * Write `nb` bits from value `v` (LSB-first) — word-at-a-time flush.
 *
 * Accumulates bits in a 64-bit register and flushes 4 bytes at once when
 * the accumulator holds ≥ 32 bits, reducing the flush branch from once per
 * symbol to once per ~3-4 symbols compared to the byte-at-a-time approach.
 *
 * nb must be 0–32. Returns 0 on success, -1 on buffer overflow.
 */
static NETC_INLINE int netc_bsw_write(netc_bsw_t *w, uint32_t v, int nb) {
    w->accum |= (uint64_t)v << w->bits;
    w->bits  += nb;
    if (w->bits >= 32) {
        if (NETC_UNLIKELY(w->ptr + 4 > w->end)) return -1;
        /* Unaligned 4-byte store — memcpy inlines to a single mov on x86/x64 */
        uint32_t word = (uint32_t)w->accum;
        memcpy(w->ptr, &word, 4);
        w->ptr   += 4;
        w->accum >>= 32;
        w->bits  -= 32;
    }
    return 0;
}

/**
 * Flush remaining bits to the buffer with a sentinel.
 *
 * Appends a sentinel 1-bit immediately after all data bits, then pads to
 * the next byte boundary with 0-bits. The sentinel allows the reader to
 * locate the valid data start within the last byte.
 *
 * Must be called exactly once after all netc_bsw_write calls.
 * Returns the total number of bytes written, or (size_t)-1 on overflow.
 */
static NETC_INLINE size_t netc_bsw_flush(netc_bsw_t *w) {
    /* Append sentinel 1-bit immediately after data */
    w->accum |= (uint64_t)1u << w->bits;
    w->bits  += 1;
    /* Flush remaining bytes one at a time (≤ 4 bytes remain after word flush) */
    while (w->bits > 0) {
        if (NETC_UNLIKELY(w->ptr >= w->end)) return (size_t)-1;
        *w->ptr++ = (uint8_t)(w->accum & 0xFFU);
        w->accum >>= 8;
        w->bits   -= 8;
    }
    return (size_t)(w->ptr - w->start);
}

/** Return bytes written so far (before flush). */
static NETC_INLINE size_t netc_bsw_size(const netc_bsw_t *w) {
    return (size_t)(w->ptr - w->start);
}

/* =========================================================================
 * Bitstream reader (MSB-first consumption, back-to-front byte order)
 *
 * The accumulator holds valid bits in its upper `bits` positions:
 *   bit 63         = next bit to consume
 *   bit (64-bits)  = last valid bit
 *   bits below     = 0 (unused)
 *
 * On init the last byte of the stream (containing the sentinel) is placed
 * in bits [63..56]. The sentinel and any padding above it are skipped so
 * that bit 63 points to the actual last encoder bit.
 * ========================================================================= */

typedef struct {
    const uint8_t *start;  /* Start of bitstream buffer */
    const uint8_t *ptr;    /* Next byte to load (moves backward toward start) */
    uint64_t       accum;  /* Bit window: MSB (bit 63) = next bit to consume */
    int            bits;   /* Number of valid bits in accum (0–64) */
} netc_bsr_t;

/* floor_log2 for a non-zero byte: position of highest set bit (0=LSB). */
static NETC_INLINE int netc_bsr__floorlog2_byte(uint8_t v) {
    int n = 0;
    if (v & 0xF0) { v >>= 4; n += 4; }
    if (v & 0x0C) { v >>= 2; n += 2; }
    if (v & 0x02) { n += 1; }
    return n;
}

/**
 * Initialize a bitstream reader over [buf, buf+size).
 *
 * Loads up to 8 bytes from the end of the stream into the MSB accumulator
 * (last stream byte occupies bits [63..56]).  Finds and discards the
 * sentinel bit so that the first netc_bsr_read returns the last encoder bit.
 */
static NETC_INLINE void netc_bsr_init(netc_bsr_t *r, const void *buf, size_t size) {
    r->start = (const uint8_t *)buf;
    r->ptr   = (const uint8_t *)buf + size;
    r->accum = 0;
    r->bits  = 0;

    if (size == 0) return;

    int fill = (size < 8) ? (int)size : 8;

    /* Load bytes from oldest to newest so the last stream byte ends up
     * in the top 8 bits of accum.
     *
     * Approach: load into a temporary bottom-aligned word, then shift. */
    const uint8_t *src = r->ptr - fill;
    uint64_t raw = 0;
    for (int i = 0; i < fill; i++) {
        raw |= (uint64_t)src[i] << (i * 8);
    }
    r->ptr -= fill;

    /* raw is LSB-aligned: raw[7..0] = bytes oldest..newest.
     * We want MSB-aligned: newest byte at bits 63..56.
     * Shift raw left by (64 - fill*8). */
    int total_bits = fill * 8;
    r->accum = raw << (64 - total_bits);
    r->bits  = total_bits;

    /* Find the sentinel: highest set bit in the last stream byte, which now
     * occupies bits [63..56] of accum.  The sentinel is the MSB of that byte
     * that is set. Leading zero bits (above the sentinel) are padding. */
    uint8_t last_byte = (uint8_t)(r->accum >> 56);
    if (last_byte == 0) {
        /* Degenerate: no sentinel found (corrupt stream) */
        r->bits = 0;
        return;
    }
    /* Number of leading zeros in last_byte (from MSB) = 7 - floor_log2(last_byte) */
    int sentinel_bit_in_byte = netc_bsr__floorlog2_byte(last_byte); /* 0=LSB */
    /* In accum, that bit is at position 56 + sentinel_bit_in_byte from LSB
     * = bit (63 - (7 - sentinel_bit_in_byte)) from MSB.
     * Number of bits to skip from the TOP of accum =
     *   leading_zeros_of_last_byte + 1 (for sentinel)
     *   = (7 - sentinel_bit_in_byte) + 1
     *   = 8 - sentinel_bit_in_byte - 1 ... wait let's be careful */
    /* leading zeros of last_byte from MSB = 7 - sentinel_bit_in_byte */
    int lead_zeros = 7 - sentinel_bit_in_byte;
    int skip = lead_zeros + 1;   /* skip padding zeros + sentinel */
    r->accum <<= skip;
    r->bits   -= skip;
    if (r->bits < 0) r->bits = 0;
}

/**
 * Peek at the next `nb` bits without consuming them.
 * Returns the nb-bit value from the TOP of the accumulator.
 * nb must be 1–32.
 */
static NETC_INLINE uint32_t netc_bsr_peek(const netc_bsr_t *r, int nb) {
    return (uint32_t)(r->accum >> (64 - nb));
}

/**
 * Consume `nb` bits from the reader, refilling the accumulator as needed.
 * Returns 0 on success, -1 on underflow (read past start of buffer).
 */
static NETC_INLINE int netc_bsr_consume(netc_bsr_t *r, int nb) {
    r->accum <<= nb;
    r->bits   -= nb;
    /* Refill accumulator from the buffer (read backwards).
     * Each new byte goes to the BOTTOM of the valid region. */
    while (r->bits <= 56 && r->ptr > r->start) {
        r->ptr--;
        /* Place new byte just below current valid bits */
        r->accum |= (uint64_t)(*r->ptr) << (56 - r->bits);
        r->bits  += 8;
    }
    return (r->bits >= 0) ? 0 : -1;
}

/**
 * Read `nb` bits (peek + consume combined).
 * Writes the value to *out. Returns 0 on success, -1 on underflow.
 */
static NETC_INLINE int netc_bsr_read(netc_bsr_t *r, int nb, uint32_t *out) {
    *out = netc_bsr_peek(r, nb);
    return netc_bsr_consume(r, nb);
}

/** Return 1 if the reader has reached or passed the start of the buffer. */
static NETC_INLINE int netc_bsr_empty(const netc_bsr_t *r) {
    return (r->bits <= 0 && r->ptr <= r->start);
}

#endif /* NETC_BITSTREAM_H */
