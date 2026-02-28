/**
 * netc_delta.h — Field-class-aware delta prediction (AD-002).
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * Delta encoding subtracts predicted byte values from actual packet bytes,
 * producing residuals with lower entropy for entropy coding. The prediction
 * strategy is field-class aware: different operations are applied based on
 * the structural role inferred from the byte's position (offset) within the
 * packet (AD-002, RFC-001 §6.2).
 *
 * Field-class mapping (by packet byte offset):
 *   [0 .. 15]    HEADER     — XOR  (flag/enum bytes, type fields)
 *   [16 .. 63]   SUBHEADER  — SUB  (sequence numbers, counters)
 *   [64 .. 255]  BODY       — XOR  (float components, vectors)
 *   [256 .. END] TAIL       — SUB  (integer payload, bulk data)
 *
 * This mapping is a heuristic, not schema-derived. It keeps netc
 * schema-agnostic while still being smarter than uniform subtraction.
 *
 * API:
 *   netc_delta_encode(prev, curr, residual, size)
 *   netc_delta_decode(prev, residual, curr, size)
 *
 * Both functions operate in-place or out-of-place.
 * For in-place decode: curr == residual is allowed.
 * For in-place encode: not safe (residual overwrites curr during XOR).
 */

#ifndef NETC_DELTA_H
#define NETC_DELTA_H

#include "../util/netc_platform.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Field-class thresholds (byte offset boundaries)
 *
 * These match the coarse context bucket boundaries in netc_tans.h
 * (NETC_CTX_HEADER=0, NETC_CTX_SUBHEADER=1, NETC_CTX_BODY=2, NETC_CTX_TAIL=3).
 * ========================================================================= */

#define NETC_DELTA_HEADER_END     16U   /* offsets 0..15  → XOR */
#define NETC_DELTA_SUBHEADER_END  64U   /* offsets 16..63 → SUB */
#define NETC_DELTA_BODY_END       256U  /* offsets 64..255 → XOR */
/* offsets 256+ → SUB */

/* =========================================================================
 * Minimum packet size for delta to be useful.
 * Below this threshold, delta is skipped (too little history signal).
 * ========================================================================= */
#define NETC_DELTA_MIN_SIZE       8U

/* =========================================================================
 * netc_delta_encode
 *
 * Compute residual[i] from prev[i] and curr[i] using the field-class
 * strategy for offset i.
 *
 * prev     — previous packet bytes (predictor), length >= size
 * curr     — current packet bytes,  length >= size
 * residual — output residuals,      length >= size (may equal prev but not curr)
 * size     — number of bytes to encode
 *
 * Returns the number of bytes written (== size).
 * ========================================================================= */
static NETC_INLINE size_t netc_delta_encode(
    const uint8_t *prev,
    const uint8_t *curr,
    uint8_t       *residual,
    size_t         size)
{
    for (size_t i = 0; i < size; i++) {
        if (i < NETC_DELTA_HEADER_END || (i >= NETC_DELTA_SUBHEADER_END && i < NETC_DELTA_BODY_END)) {
            /* XOR strategy: HEADER and BODY regions */
            residual[i] = curr[i] ^ prev[i];
        } else {
            /* SUB strategy: SUBHEADER and TAIL regions (wrapping subtraction) */
            residual[i] = (uint8_t)(curr[i] - prev[i]);
        }
    }
    return size;
}

/* =========================================================================
 * netc_delta_decode
 *
 * Reconstruct curr[i] from prev[i] and residual[i].
 * Inverse of netc_delta_encode.
 *
 * prev     — previous packet bytes (predictor), length >= size
 * residual — encoded residuals,                 length >= size
 * curr     — output reconstructed bytes,        length >= size (may equal residual)
 * size     — number of bytes to decode
 *
 * Returns the number of bytes written (== size).
 * ========================================================================= */
static NETC_INLINE size_t netc_delta_decode(
    const uint8_t *prev,
    const uint8_t *residual,
    uint8_t       *curr,
    size_t         size)
{
    for (size_t i = 0; i < size; i++) {
        if (i < NETC_DELTA_HEADER_END || (i >= NETC_DELTA_SUBHEADER_END && i < NETC_DELTA_BODY_END)) {
            /* XOR is self-inverse */
            curr[i] = residual[i] ^ prev[i];
        } else {
            /* ADD to undo the subtraction */
            curr[i] = (uint8_t)(residual[i] + prev[i]);
        }
    }
    return size;
}

/* =========================================================================
 * netc_delta_encode_order2
 *
 * Order-2 delta: linear extrapolation prediction.
 *   predicted[i] = 2*prev[i] - prev2[i]  (clamped to uint8_t via wrapping)
 *   residual[i]  = curr[i] - predicted[i] (XOR or SUB per field class)
 *
 * This captures linear trends (e.g. monotonic counters, smooth position
 * changes) more accurately than order-1, producing smaller residuals.
 * ========================================================================= */
static NETC_INLINE size_t netc_delta_encode_order2(
    const uint8_t *prev2,
    const uint8_t *prev,
    const uint8_t *curr,
    uint8_t       *residual,
    size_t         size)
{
    for (size_t i = 0; i < size; i++) {
        uint8_t predicted = (uint8_t)(2U * prev[i] - prev2[i]);
        if (i < NETC_DELTA_HEADER_END || (i >= NETC_DELTA_SUBHEADER_END && i < NETC_DELTA_BODY_END)) {
            residual[i] = curr[i] ^ predicted;
        } else {
            residual[i] = (uint8_t)(curr[i] - predicted);
        }
    }
    return size;
}

/* =========================================================================
 * netc_delta_decode_order2
 *
 * Inverse of netc_delta_encode_order2.
 *   predicted[i] = 2*prev[i] - prev2[i]
 *   curr[i]      = residual[i] + predicted[i] (XOR or ADD per field class)
 * ========================================================================= */
static NETC_INLINE size_t netc_delta_decode_order2(
    const uint8_t *prev2,
    const uint8_t *prev,
    const uint8_t *residual,
    uint8_t       *curr,
    size_t         size)
{
    for (size_t i = 0; i < size; i++) {
        uint8_t predicted = (uint8_t)(2U * prev[i] - prev2[i]);
        if (i < NETC_DELTA_HEADER_END || (i >= NETC_DELTA_SUBHEADER_END && i < NETC_DELTA_BODY_END)) {
            curr[i] = residual[i] ^ predicted;
        } else {
            curr[i] = (uint8_t)(residual[i] + predicted);
        }
    }
    return size;
}

#endif /* NETC_DELTA_H */
