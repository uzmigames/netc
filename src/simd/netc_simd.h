/**
 * netc_simd.h — SIMD capability detection and dispatch table.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * Provides:
 *   - Runtime SIMD capability detection (CPUID on x86, AT_HWCAP on Linux/ARM)
 *   - A dispatch table (netc_simd_ops_t) that selects the best implementation
 *     at context creation time — zero overhead in the hot path
 *   - Implementations: generic (C11), SSE4.2, AVX2, NEON
 *
 * All implementations produce byte-for-byte identical output.
 * All loads/stores are unaligned-safe (loadu / storeu variants).
 *
 * The simd_level field in netc_cfg_t maps to:
 *   0 = auto-detect (default)
 *   1 = generic (force C fallback)
 *   2 = SSE4.2
 *   3 = AVX2
 *   4 = NEON
 */

#ifndef NETC_SIMD_H
#define NETC_SIMD_H

#include "../util/netc_platform.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * SIMD level constants (matches netc_cfg_t.simd_level)
 * ========================================================================= */

#define NETC_SIMD_LEVEL_AUTO    0U
#define NETC_SIMD_LEVEL_GENERIC 1U
#define NETC_SIMD_LEVEL_SSE42   2U
#define NETC_SIMD_LEVEL_AVX2    3U
#define NETC_SIMD_LEVEL_NEON    4U

/* =========================================================================
 * Dispatch table — function pointers for bulk SIMD operations
 * ========================================================================= */

/**
 * delta_encode_bulk: encode len bytes of residuals from prev/curr into out.
 * Equivalent to netc_delta_encode but may use wider vector ops.
 * Handles any len (scalar tail for unaligned remainder).
 */
typedef void (*netc_delta_encode_fn)(const uint8_t *prev,
                                     const uint8_t *curr,
                                     uint8_t       *out,
                                     size_t         len);

/**
 * delta_decode_bulk: reconstruct curr bytes from prev + residuals.
 * in-place: out == residual is allowed.
 */
typedef void (*netc_delta_decode_fn)(const uint8_t *prev,
                                     const uint8_t *residual,
                                     uint8_t       *out,
                                     size_t         len);

/**
 * freq_count: accumulate byte frequency histogram.
 * freq[256] is ADDED to (not initialized) so caller may clear or aggregate.
 */
typedef void (*netc_freq_count_fn)(const uint8_t *data,
                                   size_t         len,
                                   uint32_t      *freq);

/**
 * crc32_update: update a running CRC32 with len bytes.
 * Returns new CRC value. Initial value is typically 0xFFFFFFFFU.
 */
typedef uint32_t (*netc_crc32_update_fn)(uint32_t crc,
                                          const uint8_t *data,
                                          size_t         len);

typedef struct {
    netc_delta_encode_fn  delta_encode;
    netc_delta_decode_fn  delta_decode;
    netc_freq_count_fn    freq_count;
    netc_crc32_update_fn  crc32_update;
    uint8_t               level;        /* actual level selected */
} netc_simd_ops_t;

/* =========================================================================
 * Capability detection
 * ========================================================================= */

/**
 * Detect available SIMD level on the current CPU.
 * Returns one of NETC_SIMD_LEVEL_*.
 */
uint8_t netc_simd_detect(void);

/* =========================================================================
 * Dispatch table initializers
 * ========================================================================= */

/**
 * Fill ops with the implementation for the given level.
 * If level > max supported on this CPU, falls back to the next lower level.
 * level == NETC_SIMD_LEVEL_AUTO → calls netc_simd_detect() first.
 */
void netc_simd_ops_init(netc_simd_ops_t *ops, uint8_t level);

/* =========================================================================
 * Generic (C11) implementations — always available
 * ========================================================================= */

void     netc_delta_encode_generic(const uint8_t *prev, const uint8_t *curr,
                                    uint8_t *out, size_t len);
void     netc_delta_decode_generic(const uint8_t *prev, const uint8_t *residual,
                                    uint8_t *out, size_t len);
void     netc_freq_count_generic  (const uint8_t *data, size_t len, uint32_t *freq);
uint32_t netc_crc32_update_generic(uint32_t crc, const uint8_t *data, size_t len);

/* =========================================================================
 * SSE4.2 implementations (compiled only when NETC_SIMD_SSE42 defined)
 * ========================================================================= */
#if defined(NETC_SIMD_SSE42) || defined(NETC_SIMD_AVX2) || defined(_MSC_VER)
/* On MSVC x64, SSE4.2 intrinsics are always available */
void     netc_delta_encode_sse42(const uint8_t *prev, const uint8_t *curr,
                                  uint8_t *out, size_t len);
void     netc_delta_decode_sse42(const uint8_t *prev, const uint8_t *residual,
                                  uint8_t *out, size_t len);
void     netc_freq_count_sse42  (const uint8_t *data, size_t len, uint32_t *freq);
uint32_t netc_crc32_update_sse42(uint32_t crc, const uint8_t *data, size_t len);
#endif

/* =========================================================================
 * AVX2 implementations (compiled only when NETC_SIMD_AVX2 defined)
 * ========================================================================= */
#if defined(NETC_SIMD_AVX2) || defined(_MSC_VER)
/* On MSVC x64, AVX2 intrinsics are available when CPU supports them */
void netc_delta_encode_avx2(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len);
void netc_delta_decode_avx2(const uint8_t *prev, const uint8_t *residual,
                              uint8_t *out, size_t len);
void netc_freq_count_avx2  (const uint8_t *data, size_t len, uint32_t *freq);
#endif

/* =========================================================================
 * NEON implementations
 * ========================================================================= */
#if defined(NETC_SIMD_NEON) || defined(__ARM_NEON)
void netc_delta_encode_neon(const uint8_t *prev, const uint8_t *curr,
                              uint8_t *out, size_t len);
void netc_delta_decode_neon(const uint8_t *prev, const uint8_t *residual,
                              uint8_t *out, size_t len);
void netc_freq_count_neon  (const uint8_t *data, size_t len, uint32_t *freq);
#endif

#endif /* NETC_SIMD_H */
