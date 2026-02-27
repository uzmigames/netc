/**
 * netc_simd_sse42.c — SSE4.2 accelerated bulk operations.
 *
 * SSE4.2 provides:
 *   - _mm_crc32_u8 / _mm_crc32_u32 / _mm_crc32_u64 — hardware CRC32C
 *   - 128-bit integer arithmetic via SSE2 (available alongside SSE4.2)
 *     _mm_sub_epi8  — 16 bytes of wrapping byte subtraction per cycle
 *     _mm_xor_si128 — 16 bytes of XOR per cycle
 *     _mm_add_epi8  — 16 bytes of wrapping byte addition per cycle
 *
 * Note: The CRC32C variant computed by SSE4.2 differs from the ISO-HDLC CRC32
 * used in the generic path. For the netc CRC32 used in dict save/load,
 * consistency matters more than the specific polynomial. We use the hardware
 * CRC32C (Castagnoli, 0x1EDC6F41 reflected = 0x82F63B78) via _mm_crc32_u*
 * and expose it via the dispatch table. The generic path uses ISO-HDLC.
 * Since the CRC is only used for dict file integrity, not wire format, mixing
 * is safe as long as the same path is used for save and load — which is
 * guaranteed because both use the context's dispatch table.
 *
 * Delta encoding: SSE4.2 does NOT change the field-class boundaries.
 * We process 16 bytes at a time in two phases:
 *   Phase A (HEADER+BODY): XOR — _mm_xor_si128
 *   Phase B (SUBHEADER+TAIL): SUB — _mm_sub_epi8
 * Scalar tail handles the last <16 bytes and exact boundary bytes.
 */

#include "netc_simd.h"
#include <stdint.h>
#include <stddef.h>

/* Include SSE4.2 intrinsics.
 * On MSVC x64: available by default (no flag needed).
 * On GCC/Clang: require -msse4.2 (set by CMake when NETC_HAS_SSE42). */
#if defined(_MSC_VER)
#  include <intrin.h>
#  include <nmmintrin.h>  /* SSE4.2 */
#  include <wmmintrin.h>
#elif defined(__SSE4_2__)
#  include <nmmintrin.h>
#  include <smmintrin.h>
#endif

/* =========================================================================
 * Field-class boundaries (same as netc_delta.h)
 * ========================================================================= */
#define HDR_END  16u   /* 0-15:   XOR */
#define SUB_END  64u   /* 16-63:  SUB */
#define BODY_END 256u  /* 64-255: XOR */
/* 256+: SUB */

/* =========================================================================
 * SSE4.2 delta encode
 *
 * Strategy: for each 16-byte chunk entirely within a single field-class
 * region, apply the vector op. Chunks that straddle a boundary fall back
 * to scalar. This keeps branch overhead low for typical packet sizes.
 * ========================================================================= */

#if defined(_MSC_VER) || defined(__SSE4_2__)

void netc_delta_encode_sse42(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len)
{
    size_t i = 0;

    /* HEADER region [0, 16): XOR, exactly one 16-byte vector if len >= 16 */
    {
        size_t end = len < HDR_END ? len : HDR_END;
        /* scalar — region is only 16 bytes, no benefit from looping */
        while (i < end) {
            out[i] = curr[i] ^ prev[i];
            i++;
        }
    }

    /* SUBHEADER region [16, 64): SUB, up to 3 × 16-byte vectors */
    {
        size_t end = len < SUB_END ? len : SUB_END;
        /* Process 16-byte chunks */
        while (i + 16u <= end) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            __m128i r = _mm_sub_epi8(c, p);
            _mm_storeu_si128((__m128i *)(out + i), r);
            i += 16u;
        }
        /* scalar tail */
        while (i < end) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }

    /* BODY region [64, 256): XOR, up to 12 × 16-byte vectors */
    {
        size_t end = len < BODY_END ? len : BODY_END;
        while (i + 16u <= end) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            __m128i r = _mm_xor_si128(c, p);
            _mm_storeu_si128((__m128i *)(out + i), r);
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
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            __m128i r = _mm_sub_epi8(c, p);
            _mm_storeu_si128((__m128i *)(out + i), r);
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }
}

void netc_delta_decode_sse42(const uint8_t *prev, const uint8_t *residual,
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
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i r = _mm_loadu_si128((const __m128i *)(residual + i));
            __m128i c = _mm_add_epi8(r, p);
            _mm_storeu_si128((__m128i *)(out + i), c);
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
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i r = _mm_loadu_si128((const __m128i *)(residual + i));
            __m128i c = _mm_xor_si128(r, p);
            _mm_storeu_si128((__m128i *)(out + i), c);
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
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i r = _mm_loadu_si128((const __m128i *)(residual + i));
            __m128i c = _mm_add_epi8(r, p);
            _mm_storeu_si128((__m128i *)(out + i), c);
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(residual[i] + prev[i]);
            i++;
        }
    }
}

/* =========================================================================
 * SSE4.2 frequency count
 *
 * Basic approach: process 16 bytes per iteration, scatter to histogram.
 * A true vectorised histogram needs VGATHERDPS or a table-splitting trick;
 * here we use a 4-way unrolled scalar scatter which is still 2-3× faster
 * than a naive single-byte loop due to out-of-order execution.
 * ========================================================================= */

void netc_freq_count_sse42(const uint8_t *data, size_t len, uint32_t *freq)
{
    size_t i = 0;

    /* Process 16 bytes at a time using SSE load then scalar scatter */
    for (; i + 16u <= len; i += 16u) {
        __m128i v = _mm_loadu_si128((const __m128i *)(data + i));
        /* Extract bytes and increment histogram */
        uint8_t b[16];
        _mm_storeu_si128((__m128i *)b, v);
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
 * SSE4.2 hardware CRC32C
 *
 * Uses the CRC32C (Castagnoli) polynomial via _mm_crc32_u* intrinsics.
 * Processes 8 bytes at a time on x64, falling back to 1-byte at tail.
 * ========================================================================= */

uint32_t netc_crc32_update_sse42(uint32_t crc, const uint8_t *data, size_t len)
{
    size_t i = 0;

#if defined(_M_X64) || defined(__x86_64__)
    /* Process 8 bytes at a time */
    uint64_t crc64 = (uint64_t)(uint32_t)crc;
    for (; i + 8u <= len; i += 8u) {
        uint64_t v;
        memcpy(&v, data + i, 8);
        crc64 = _mm_crc32_u64(crc64, v);
    }
    crc = (uint32_t)crc64;
#endif

    /* 4-byte chunks */
    for (; i + 4u <= len; i += 4u) {
        uint32_t v;
        memcpy(&v, data + i, 4);
        crc = _mm_crc32_u32(crc, v);
    }

    /* Byte tail */
    for (; i < len; i++) {
        crc = _mm_crc32_u8(crc, data[i]);
    }

    return crc;
}

#else /* SSE4.2 not available at compile time — stubs that should never be called */

void netc_delta_encode_sse42(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len)
{
    netc_delta_encode_generic(prev, curr, out, len);
}
void netc_delta_decode_sse42(const uint8_t *prev, const uint8_t *residual,
                              uint8_t *out, size_t len)
{
    netc_delta_decode_generic(prev, residual, out, len);
}
void netc_freq_count_sse42(const uint8_t *data, size_t len, uint32_t *freq)
{
    netc_freq_count_generic(data, len, freq);
}
uint32_t netc_crc32_update_sse42(uint32_t crc, const uint8_t *data, size_t len)
{
    return netc_crc32_update_generic(crc, data, len);
}

#endif /* SSE4.2 */
