/**
 * netc_simd_avx2.c — AVX2 accelerated bulk operations.
 *
 * AVX2 extends the 128-bit SSE2 operations to 256-bit ymm registers:
 *   _mm256_sub_epi8  — 32 bytes of wrapping byte subtraction per cycle
 *   _mm256_xor_si256 — 32 bytes of XOR per cycle
 *   _mm256_add_epi8  — 32 bytes of wrapping byte addition per cycle
 *
 * Delta throughput: 32 bytes/cycle vs 16 bytes/cycle for SSE4.2.
 * For a 512-byte game state packet this is ~16 cycles vs ~32 cycles
 * for the BODY+TAIL regions.
 *
 * For CRC32 we fall back to the SSE4.2 hardware CRC32C — AVX2 does not
 * add new CRC instructions.
 *
 * Unaligned loads/stores (_mm256_loadu_si256 / _mm256_storeu_si256) are
 * used throughout to handle buffers at any alignment.
 */

#include "netc_simd.h"
#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#  include <immintrin.h>  /* AVX, AVX2 */
#elif defined(__AVX2__)
#  include <immintrin.h>
#endif

/* =========================================================================
 * Field-class boundaries (same as netc_delta.h and sse42)
 * ========================================================================= */
#define HDR_END  16u
#define SUB_END  64u
#define BODY_END 256u

/* =========================================================================
 * AVX2 helper: check if CPU actually supports AVX2 at runtime.
 * We use _xgetbv and CPUID — same checks as in netc_simd_generic.c.
 * These functions are compiled into this TU but only called from the
 * dispatch path which has already verified capability.
 * ========================================================================= */

#if defined(_MSC_VER) || defined(__AVX2__)

void netc_delta_encode_avx2(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len)
{
    size_t i = 0;

    /* HEADER [0, 16): XOR — scalar (region < 32 bytes, vector not worth it) */
    {
        size_t end = len < HDR_END ? len : HDR_END;
        while (i < end) {
            out[i] = curr[i] ^ prev[i];
            i++;
        }
    }

    /* SUBHEADER [16, 64): SUB — up to 3 × 16-byte SSE2 ops or scalar */
    {
        size_t end = len < SUB_END ? len : SUB_END;
        /* 48-byte region — use SSE2 (128-bit), AVX2 would straddle boundary */
        while (i + 16u <= end) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            _mm_storeu_si128((__m128i *)(out + i), _mm_sub_epi8(c, p));
            i += 16u;
        }
        while (i < end) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }

    /* BODY [64, 256): XOR — up to 6 × 32-byte AVX2 ops */
    {
        size_t end = len < BODY_END ? len : BODY_END;
        while (i + 32u <= end) {
            __m256i p = _mm256_loadu_si256((const __m256i *)(prev + i));
            __m256i c = _mm256_loadu_si256((const __m256i *)(curr + i));
            _mm256_storeu_si256((__m256i *)(out + i), _mm256_xor_si256(c, p));
            i += 32u;
        }
        /* 16-byte tail within BODY */
        while (i + 16u <= end) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            _mm_storeu_si128((__m128i *)(out + i), _mm_xor_si128(c, p));
            i += 16u;
        }
        while (i < end) {
            out[i] = curr[i] ^ prev[i];
            i++;
        }
    }

    /* TAIL [256, len): SUB — 32 bytes/iter */
    {
        while (i + 32u <= len) {
            __m256i p = _mm256_loadu_si256((const __m256i *)(prev + i));
            __m256i c = _mm256_loadu_si256((const __m256i *)(curr + i));
            _mm256_storeu_si256((__m256i *)(out + i), _mm256_sub_epi8(c, p));
            i += 32u;
        }
        while (i + 16u <= len) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i c = _mm_loadu_si128((const __m128i *)(curr + i));
            _mm_storeu_si128((__m128i *)(out + i), _mm_sub_epi8(c, p));
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(curr[i] - prev[i]);
            i++;
        }
    }
}

void netc_delta_decode_avx2(const uint8_t *prev, const uint8_t *residual,
                              uint8_t *out, size_t len)
{
    size_t i = 0;

    /* HEADER [0, 16): XOR — scalar */
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
            _mm_storeu_si128((__m128i *)(out + i), _mm_add_epi8(r, p));
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
        while (i + 32u <= end) {
            __m256i p = _mm256_loadu_si256((const __m256i *)(prev + i));
            __m256i r = _mm256_loadu_si256((const __m256i *)(residual + i));
            _mm256_storeu_si256((__m256i *)(out + i), _mm256_xor_si256(r, p));
            i += 32u;
        }
        while (i + 16u <= end) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i r = _mm_loadu_si128((const __m128i *)(residual + i));
            _mm_storeu_si128((__m128i *)(out + i), _mm_xor_si128(r, p));
            i += 16u;
        }
        while (i < end) {
            out[i] = residual[i] ^ prev[i];
            i++;
        }
    }

    /* TAIL [256, len): ADD */
    {
        while (i + 32u <= len) {
            __m256i p = _mm256_loadu_si256((const __m256i *)(prev + i));
            __m256i r = _mm256_loadu_si256((const __m256i *)(residual + i));
            _mm256_storeu_si256((__m256i *)(out + i), _mm256_add_epi8(r, p));
            i += 32u;
        }
        while (i + 16u <= len) {
            __m128i p = _mm_loadu_si128((const __m128i *)(prev + i));
            __m128i r = _mm_loadu_si128((const __m128i *)(residual + i));
            _mm_storeu_si128((__m128i *)(out + i), _mm_add_epi8(r, p));
            i += 16u;
        }
        while (i < len) {
            out[i] = (uint8_t)(residual[i] + prev[i]);
            i++;
        }
    }
}

/* =========================================================================
 * AVX2 frequency count
 *
 * Uses four 256-entry partial histograms to reduce store-forwarding stalls.
 * Each histogram counts every 4th byte of the input.
 * ========================================================================= */
void netc_freq_count_avx2(const uint8_t *data, size_t len, uint32_t *freq)
{
    /* 4-way unrolled scalar using AVX2 loads for cache efficiency */
    uint32_t h0[256] = {0}, h1[256] = {0}, h2[256] = {0}, h3[256] = {0};
    size_t i = 0;

    for (; i + 32u <= len; i += 32u) {
        uint8_t b[32];
        __m256i v = _mm256_loadu_si256((const __m256i *)(data + i));
        _mm256_storeu_si256((__m256i *)b, v);
        h0[b[ 0]]++; h1[b[ 1]]++; h2[b[ 2]]++; h3[b[ 3]]++;
        h0[b[ 4]]++; h1[b[ 5]]++; h2[b[ 6]]++; h3[b[ 7]]++;
        h0[b[ 8]]++; h1[b[ 9]]++; h2[b[10]]++; h3[b[11]]++;
        h0[b[12]]++; h1[b[13]]++; h2[b[14]]++; h3[b[15]]++;
        h0[b[16]]++; h1[b[17]]++; h2[b[18]]++; h3[b[19]]++;
        h0[b[20]]++; h1[b[21]]++; h2[b[22]]++; h3[b[23]]++;
        h0[b[24]]++; h1[b[25]]++; h2[b[26]]++; h3[b[27]]++;
        h0[b[28]]++; h1[b[29]]++; h2[b[30]]++; h3[b[31]]++;
    }
    for (; i < len; i++) {
        h0[data[i]]++;
    }

    /* Merge partial histograms */
    for (int k = 0; k < 256; k++) {
        freq[k] += h0[k] + h1[k] + h2[k] + h3[k];
    }
}

#else /* AVX2 not available at compile time */

void netc_delta_encode_avx2(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len)
{
    netc_delta_encode_generic(prev, curr, out, len);
}
void netc_delta_decode_avx2(const uint8_t *prev, const uint8_t *residual,
                              uint8_t *out, size_t len)
{
    netc_delta_decode_generic(prev, residual, out, len);
}
void netc_freq_count_avx2(const uint8_t *data, size_t len, uint32_t *freq)
{
    netc_freq_count_generic(data, len, freq);
}

#endif /* AVX2 */
