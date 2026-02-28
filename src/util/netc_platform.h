/**
 * netc_platform.h — Portability abstraction for compiler/platform specifics.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * Isolates all uses of _Atomic, _Alignas, branch hints, and force-inline
 * behind macros so the rest of the codebase remains clean C11 (AD-008).
 *
 * MSVC caveat: MSVC C11 _Atomic support is incomplete. This header provides
 * a safe fallback for MSVC using volatile + Interlocked functions.
 */

#ifndef NETC_PLATFORM_H
#define NETC_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* =========================================================================
 * Compiler detection
 * ========================================================================= */

#if defined(_MSC_VER)
#  define NETC_COMPILER_MSVC 1
#elif defined(__clang__)
#  define NETC_COMPILER_CLANG 1
#elif defined(__GNUC__)
#  define NETC_COMPILER_GCC 1
#endif

/* =========================================================================
 * NETC_INLINE — force inline
 * ========================================================================= */

#if defined(NETC_COMPILER_MSVC)
#  define NETC_INLINE __forceinline
#elif defined(NETC_COMPILER_GCC) || defined(NETC_COMPILER_CLANG)
#  define NETC_INLINE __attribute__((always_inline)) inline
#else
#  define NETC_INLINE inline
#endif

/* =========================================================================
 * NETC_NOINLINE — prevent inlining (for cold paths)
 * ========================================================================= */

#if defined(NETC_COMPILER_MSVC)
#  define NETC_NOINLINE __declspec(noinline)
#elif defined(NETC_COMPILER_GCC) || defined(NETC_COMPILER_CLANG)
#  define NETC_NOINLINE __attribute__((noinline))
#else
#  define NETC_NOINLINE
#endif

/* =========================================================================
 * NETC_MAYBE_UNUSED — suppress unused-function/variable warnings
 * ========================================================================= */

#if defined(NETC_COMPILER_GCC) || defined(NETC_COMPILER_CLANG)
#  define NETC_MAYBE_UNUSED __attribute__((unused))
#elif defined(NETC_COMPILER_MSVC)
#  define NETC_MAYBE_UNUSED
#else
#  define NETC_MAYBE_UNUSED
#endif

/* =========================================================================
 * NETC_ALIGN(N) — alignment attribute
 * ========================================================================= */

#if defined(NETC_COMPILER_MSVC)
#  define NETC_ALIGN(N) __declspec(align(N))
#else
#  define NETC_ALIGN(N) _Alignas(N)
#endif

/* =========================================================================
 * NETC_LIKELY / NETC_UNLIKELY — branch prediction hints
 * ========================================================================= */

#if defined(NETC_COMPILER_GCC) || defined(NETC_COMPILER_CLANG)
#  define NETC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define NETC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define NETC_LIKELY(x)   (x)
#  define NETC_UNLIKELY(x) (x)
#endif

/* =========================================================================
 * NETC_ATOMIC(T) — atomic type
 * MSVC: fall back to volatile (sufficient for our single-writer patterns)
 * ========================================================================= */

#if defined(NETC_COMPILER_MSVC)
#  define NETC_ATOMIC(T) volatile T
#else
#  define NETC_ATOMIC(T) _Atomic T
#endif

/* =========================================================================
 * NETC_PREFETCH — software prefetch hint (read, L1 locality)
 *
 * Used to hide memory latency in hot loops by issuing a prefetch for the
 * next iteration's data while the current iteration computes.
 * No-op on compilers/platforms that don't support it.
 * ========================================================================= */

#if defined(NETC_COMPILER_MSVC)
#  include <intrin.h>
#  define NETC_PREFETCH(ptr) _mm_prefetch((const char *)(ptr), _MM_HINT_T0)
#elif defined(NETC_COMPILER_GCC) || defined(NETC_COMPILER_CLANG)
#  define NETC_PREFETCH(ptr) __builtin_prefetch((ptr), 0, 3)
#else
#  define NETC_PREFETCH(ptr) ((void)(ptr))
#endif

/* =========================================================================
 * NETC_STATIC_ASSERT — compile-time assertion
 * ========================================================================= */

#define NETC_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* =========================================================================
 * Endianness — little-endian primary (RFC-001 §14)
 * ========================================================================= */

#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define NETC_LITTLE_ENDIAN 1
#endif

/** Read uint16 from unaligned little-endian bytes. */
static NETC_INLINE uint16_t netc_read_u16_le(const void *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
#if !defined(NETC_LITTLE_ENDIAN)
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

/** Write uint16 to unaligned little-endian bytes. */
static NETC_INLINE void netc_write_u16_le(void *p, uint16_t v) {
#if !defined(NETC_LITTLE_ENDIAN)
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    memcpy(p, &v, sizeof(v));
}

/** Read uint32 from unaligned little-endian bytes. */
static NETC_INLINE uint32_t netc_read_u32_le(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
#if !defined(NETC_LITTLE_ENDIAN)
    v = ((v & 0xFF000000U) >> 24) | ((v & 0x00FF0000U) >> 8)
      | ((v & 0x0000FF00U) << 8)  | ((v & 0x000000FFU) << 24);
#endif
    return v;
}

/** Write uint32 to unaligned little-endian bytes. */
static NETC_INLINE void netc_write_u32_le(void *p, uint32_t v) {
#if !defined(NETC_LITTLE_ENDIAN)
    v = ((v & 0xFF000000U) >> 24) | ((v & 0x00FF0000U) >> 8)
      | ((v & 0x0000FF00U) << 8)  | ((v & 0x000000FFU) << 24);
#endif
    memcpy(p, &v, sizeof(v));
}

#endif /* NETC_PLATFORM_H */
