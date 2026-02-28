/**
 * netc_simd_neon.c — ARM NEON accelerated bulk operations.
 *
 * NEON provides 128-bit SIMD (same width as SSE2) on AArch64:
 *   - vsubq_u8  — 16 bytes of wrapping byte subtraction
 *   - veorq_u8  — 16 bytes of XOR
 *   - vaddq_u8  — 16 bytes of wrapping byte addition
 *   - vld1q_u8 / vst1q_u8 — unaligned 128-bit load/store
 *
 * ARMv8.1+ CRC32 extension (when __ARM_FEATURE_CRC32 defined):
 *   - __crc32b / __crc32w / __crc32d — hardware CRC32 (ISO-HDLC polynomial)
 *
 * Delta encoding uses the same 4-region field-class strategy as SSE4.2:
 *   HEADER   [0, 16):   XOR  (scalar — only 16 bytes)
 *   SUBHEADER[16, 64):  SUB  (vsubq_u8, 16 bytes/iter)
 *   BODY     [64, 256): XOR  (veorq_u8, 16 bytes/iter)
 *   TAIL     [256+):    SUB  (vsubq_u8, 16 bytes/iter)
 */

#include "netc_simd.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

#if defined(__ARM_FEATURE_CRC32)
#  include <arm_acle.h>
#endif

/* =========================================================================
 * Field-class boundaries (same as netc_delta.h and netc_simd_sse42.c)
 * ========================================================================= */
#define HDR_END  16u   /* 0-15:   XOR */
#define SUB_END  64u   /* 16-63:  SUB */
#define BODY_END 256u  /* 64-255: XOR */
/* 256+: SUB */

/* =========================================================================
 * NEON implementations (when ARM NEON is available)
 * ========================================================================= */

#if defined(__ARM_NEON)

void netc_delta_encode_neon(const uint8_t *prev, const uint8_t *curr,
                             uint8_t *out, size_t len)
{
    size_t i = 0;

    /* HEADER region [0, 16): XOR, scalar (only 16 bytes) */
    {
        size_t end = len < HDR_END ? len : HDR_END;
        while (i < end) {
            out[i] = curr[i] ^ prev[i];
            i++;
        }
    }

    /* SUBHEADER region [16, 64): SUB, up to 3 × 16-byte vectors */
    {
        size_t end = len < SUB_END ? len : SUB_END;
        while (i + 16u <= end) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t c = vld1q_u8(curr + i);
            uint8x16_t r = vsubq_u8(c, p);
            vst1q_u8(out + i, r);
            i += 16u;
        }
        while (i < end) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }

    /* BODY region [64, 256): XOR, up to 12 × 16-byte vectors */
    {
        size_t end = len < BODY_END ? len : BODY_END;
        while (i + 16u <= end) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t c = vld1q_u8(curr + i);
            uint8x16_t r = veorq_u8(c, p);
            vst1q_u8(out + i, r);
            i += 16u;
        }
        while (i < end) {
            out[i] = curr[i] ^ prev[i];
            i++;
        }
    }

    /* TAIL region [256, len): SUB */
    {
        while (i + 16u <= len) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t c = vld1q_u8(curr + i);
            uint8x16_t r = vsubq_u8(c, p);
            vst1q_u8(out + i, r);
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }
}

void netc_delta_decode_neon(const uint8_t *prev, const uint8_t *residual,
                             uint8_t *out, size_t len)
{
    size_t i = 0;

    /* HEADER [0, 16): XOR */
    {
        size_t end = len < HDR_END ? len : HDR_END;
        while (i < end) {
            out[i] = residual[i] ^ prev[i];
            i++;
        }
    }

    /* SUBHEADER [16, 64): ADD */
    {
        size_t end = len < SUB_END ? len : SUB_END;
        while (i + 16u <= end) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t r = vld1q_u8(residual + i);
            uint8x16_t c = vaddq_u8(r, p);
            vst1q_u8(out + i, c);
            i += 16u;
        }
        while (i < end) {
            out[i] = (uint8_t)(residual[i] + prev[i]);
            i++;
        }
    }

    /* BODY [64, 256): XOR */
    {
        size_t end = len < BODY_END ? len : BODY_END;
        while (i + 16u <= end) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t r = vld1q_u8(residual + i);
            uint8x16_t c = veorq_u8(r, p);
            vst1q_u8(out + i, c);
            i += 16u;
        }
        while (i < end) {
            out[i] = residual[i] ^ prev[i];
            i++;
        }
    }

    /* TAIL [256, len): ADD */
    {
        while (i + 16u <= len) {
            uint8x16_t p = vld1q_u8(prev + i);
            uint8x16_t r = vld1q_u8(residual + i);
            uint8x16_t c = vaddq_u8(r, p);
            vst1q_u8(out + i, c);
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(residual[i] + prev[i]);
            i++;
        }
    }
}

/* =========================================================================
 * NEON frequency count
 *
 * Same approach as SSE4.2: load 16 bytes via vector, store to temp array,
 * scatter to histogram. NEON has no gather/scatter for histograms.
 * ========================================================================= */

void netc_freq_count_neon(const uint8_t *data, size_t len, uint32_t *freq)
{
    size_t i = 0;

    /* Process 16 bytes at a time using NEON load then scalar scatter */
    for (; i + 16u <= len; i += 16u) {
        uint8x16_t v = vld1q_u8(data + i);
        uint8_t b[16];
        vst1q_u8(b, v);
        freq[b[ 0]]++; freq[b[ 1]]++; freq[b[ 2]]++; freq[b[ 3]]++;
        freq[b[ 4]]++; freq[b[ 5]]++; freq[b[ 6]]++; freq[b[ 7]]++;
        freq[b[ 8]]++; freq[b[ 9]]++; freq[b[10]]++; freq[b[11]]++;
        freq[b[12]]++; freq[b[13]]++; freq[b[14]]++; freq[b[15]]++;
    }

    /* Scalar tail */
    for (; i < len; i++) {
        freq[data[i]]++;
    }
}

/* =========================================================================
 * NEON CRC32 — hardware CRC32 on ARMv8.1+ (ISO-HDLC polynomial)
 *
 * Uses __crc32b / __crc32w / __crc32d from <arm_acle.h> when available.
 * These use the standard ISO-HDLC/CRC32 polynomial (0x04C11DB7), NOT
 * CRC32C (Castagnoli). This matches the generic software CRC32 path.
 *
 * Falls back to generic when __ARM_FEATURE_CRC32 is not defined.
 * ========================================================================= */

#if defined(__ARM_FEATURE_CRC32)

uint32_t netc_crc32_update_neon(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i = 0;

    /* Process 8 bytes at a time on AArch64 */
#if defined(__aarch64__) || defined(_M_ARM64)
    for (; i + 8u <= len; i += 8u) {
        uint64_t v;
        memcpy(&v, data + i, 8);
        crc = (uint32_t)__crc32d(crc, v);
    }
#endif

    /* 4-byte chunks */
    for (; i + 4u <= len; i += 4u) {
        uint32_t v;
        memcpy(&v, data + i, 4);
        crc = __crc32w(crc, v);
    }

    /* Byte tail */
    for (; i < len; i++) {
        crc = __crc32b(crc, data[i]);
    }

    return crc;
}

#else /* No hardware CRC32 — delegate to generic */

uint32_t netc_crc32_update_neon(uint32_t crc, const uint8_t *data, size_t len)
{
    return netc_crc32_update_generic(crc, data, len);
}

#endif /* __ARM_FEATURE_CRC32 */

#else /* __ARM_NEON not defined — stubs that delegate to generic */

void netc_delta_encode_neon(const uint8_t *prev, const uint8_t *curr,
                             uint8_t *out, size_t len)
{
    netc_delta_encode_generic(prev, curr, out, len);
}

void netc_delta_decode_neon(const uint8_t *prev, const uint8_t *residual,
                             uint8_t *out, size_t len)
{
    netc_delta_decode_generic(prev, residual, out, len);
}

void netc_freq_count_neon(const uint8_t *data, size_t len, uint32_t *freq)
{
    netc_freq_count_generic(data, len, freq);
}

uint32_t netc_crc32_update_neon(uint32_t crc, const uint8_t *data, size_t len)
{
    return netc_crc32_update_generic(crc, data, len);
}

#endif /* __ARM_NEON */
