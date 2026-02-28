/**
 * netc_simd_generic.c — Generic (C11) SIMD-equivalent implementations.
 *
 * Contains:
 *   1. Runtime SIMD capability detection (CPUID / AT_HWCAP)
 *   2. netc_simd_ops_init() dispatch table initializer
 *   3. Generic C11 fallback implementations for all bulk ops
 *
 * The generic implementations produce identical output to SIMD paths.
 * They also handle the scalar tail of SIMD paths (remainder after
 * processing full vector-width chunks).
 *
 * Note on delta field-class logic: the generic bulk ops apply a flat
 * XOR for the entire buffer. The field-class-aware logic (AD-002) is
 * applied at the pipeline level in netc_compress/netc_decompress via
 * netc_delta.h. The SIMD bulk ops here are used for the flat inner
 * loop; field-class boundary handling remains scalar.
 */

#include "netc_simd.h"
#include "../util/netc_crc32.h"
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * CPUID / capability detection
 * ========================================================================= */

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#  include <intrin.h>
#  define NETC_HAVE_CPUID 1
#elif defined(__GNUC__) || defined(__clang__)
#  if defined(__x86_64__) || defined(__i386__)
#    include <cpuid.h>
#    define NETC_HAVE_CPUID 1
#  endif
#endif

/* Check SSE4.2 (CPUID leaf 1, ECX bit 20) */
static int netc__has_sse42(void) {
#if defined(NETC_HAVE_CPUID) && (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#  if defined(_MSC_VER)
    int info[4];
    __cpuid(info, 1);
    return (info[2] >> 20) & 1;
#  else
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return (ecx >> 20) & 1;
    }
    return 0;
#  endif
#else
    return 0;
#endif
}

/* Check AVX2 (CPUID leaf 7 sub-leaf 0, EBX bit 5)
 * Also requires OSXSAVE (leaf 1, ECX bit 27) + XGETBV YMM save. */
static int netc__has_avx2(void) {
#if defined(NETC_HAVE_CPUID) && (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#  if defined(_MSC_VER)
    int info[4];
    /* Check AVX (leaf 1, ECX bit 28) + OSXSAVE (bit 27) */
    __cpuid(info, 1);
    if (!((info[2] >> 27) & 1)) return 0; /* OSXSAVE not set */
    if (!((info[2] >> 28) & 1)) return 0; /* AVX not set */
    /* Check OS has saved YMM state via XGETBV */
    if ((_xgetbv(0) & 0x6) != 0x6) return 0;
    /* Check AVX2 (leaf 7, EBX bit 5) */
    __cpuidex(info, 7, 0);
    return (info[1] >> 5) & 1;
#  else
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    if (!((ecx >> 27) & 1)) return 0; /* OSXSAVE */
    if (!((ecx >> 28) & 1)) return 0; /* AVX */
    /* XGETBV: use __builtin_ia32_xgetbv if available */
#    if defined(__GNUC__) && defined(__AVX__)
    if ((__builtin_ia32_xgetbv(0) & 0x6) != 0x6) return 0;
#    endif
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx >> 5) & 1;
    }
    return 0;
#  endif
#else
    return 0;
#endif
}

/* NEON detection: on AArch64, NEON is mandatory. */
static int netc__has_neon(void) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    return 1;
#elif defined(__linux__) && defined(__arm__)
    /* AT_HWCAP check would go here for 32-bit ARM; omit for now */
    return 0;
#else
    return 0;
#endif
}

/* =========================================================================
 * netc_simd_detect
 * ========================================================================= */

uint8_t netc_simd_detect(void) {
    if (netc__has_avx2())  return NETC_SIMD_LEVEL_AVX2;
    if (netc__has_sse42()) return NETC_SIMD_LEVEL_SSE42;
    if (netc__has_neon())  return NETC_SIMD_LEVEL_NEON;
    return NETC_SIMD_LEVEL_GENERIC;
}

/* =========================================================================
 * netc_simd_ops_init — fill dispatch table
 * ========================================================================= */

void netc_simd_ops_init(netc_simd_ops_t *ops, uint8_t level) {
    if (level == NETC_SIMD_LEVEL_AUTO) {
        level = netc_simd_detect();
    }

#if defined(_MSC_VER) || defined(NETC_SIMD_AVX2)
    if (level >= NETC_SIMD_LEVEL_AVX2 && netc__has_avx2()) {
        ops->delta_encode = netc_delta_encode_avx2;
        ops->delta_decode = netc_delta_decode_avx2;
        ops->freq_count   = netc_freq_count_avx2;
        ops->crc32_update = netc_crc32_update_sse42; /* AVX2 doesn't add new CRC */
        ops->level        = NETC_SIMD_LEVEL_AVX2;
        return;
    }
#endif

#if defined(_MSC_VER) || defined(NETC_SIMD_SSE42)
    if (level >= NETC_SIMD_LEVEL_SSE42 && netc__has_sse42()) {
        ops->delta_encode = netc_delta_encode_sse42;
        ops->delta_decode = netc_delta_decode_sse42;
        ops->freq_count   = netc_freq_count_sse42;
        ops->crc32_update = netc_crc32_update_sse42;
        ops->level        = NETC_SIMD_LEVEL_SSE42;
        return;
    }
#endif

#if defined(__ARM_NEON) || defined(NETC_SIMD_NEON)
    if (level >= NETC_SIMD_LEVEL_NEON && netc__has_neon()) {
        ops->delta_encode = netc_delta_encode_neon;
        ops->delta_decode = netc_delta_decode_neon;
        ops->freq_count   = netc_freq_count_neon;
        ops->crc32_update = netc_crc32_update_neon;
        ops->level        = NETC_SIMD_LEVEL_NEON;
        return;
    }
#endif

    /* Generic fallback */
    ops->delta_encode = netc_delta_encode_generic;
    ops->delta_decode = netc_delta_decode_generic;
    ops->freq_count   = netc_freq_count_generic;
    ops->crc32_update = netc_crc32_update_generic;
    ops->level        = NETC_SIMD_LEVEL_GENERIC;
}

/* =========================================================================
 * Generic C11 implementations
 * ========================================================================= */

/* --- Delta encode (flat XOR for the bulk; field-class boundaries handled
 *     at the pipeline level in netc_compress via netc_delta.h) ---
 *
 * For the SIMD bulk path, we use a simple flat XOR across all bytes.
 * The pipeline in netc_compress applies field-class-aware encoding using
 * the scalar netc_delta_encode() which handles the boundary conditions.
 * These SIMD functions are used when the pipeline wants maximum throughput
 * on large uniform regions.
 *
 * For now, to maintain exact byte-for-byte compatibility with the scalar
 * pipeline, the generic bulk functions replicate the field-class logic. */

void netc_delta_encode_generic(const uint8_t *prev, const uint8_t *curr,
                                uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        /* Replicate field-class logic from netc_delta.h */
        if (i < 16u || (i >= 64u && i < 256u)) {
            out[i] = curr[i] ^ prev[i];
        } else {
            out[i] = (uint8_t)(curr[i] - prev[i]);
        }
    }
}

void netc_delta_decode_generic(const uint8_t *prev, const uint8_t *residual,
                                uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i < 16u || (i >= 64u && i < 256u)) {
            out[i] = residual[i] ^ prev[i];
        } else {
            out[i] = (uint8_t)(residual[i] + prev[i]);
        }
    }
}

/* --- Frequency count --- */
void netc_freq_count_generic(const uint8_t *data, size_t len, uint32_t *freq)
{
    for (size_t i = 0; i < len; i++) {
        freq[data[i]]++;
    }
}

/* --- CRC32 (IEEE 802.3) ---
 * Delegates to the canonical implementation in netc_crc32.c.
 * This eliminates the duplicate lookup table that was previously here. */

uint32_t netc_crc32_update_generic(uint32_t crc, const uint8_t *data, size_t len)
{
    return netc_crc32_continue(crc, data, len);
}
