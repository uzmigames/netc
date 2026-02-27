/**
 * bench_timer.h — High-resolution timing for the benchmark harness.
 *
 * Provides nanosecond-resolution timing via:
 *   - Windows: QueryPerformanceCounter (QPC) — reliable, no RDTSC drift
 *   - Linux/macOS: clock_gettime(CLOCK_MONOTONIC_RAW)
 *   - x86 fallback: RDTSC with TSC frequency calibration
 *
 * Usage:
 *   bench_timer_init();          // call once at startup
 *   uint64_t t0 = bench_now_ns();
 *   // ... work ...
 *   uint64_t t1 = bench_now_ns();
 *   double elapsed_ns = (double)(t1 - t0);
 */

#ifndef BENCH_TIMER_H
#define BENCH_TIMER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Platform implementation — all functions are static inline.
 * Include this header in every .c that needs timing; no .c file required.
 * ========================================================================= */

#if defined(_WIN32)

#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

static inline void bench_timer_init(void) {
    /* QPC always available on Windows Vista+ */
}

static inline uint64_t bench_now_ns(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    /* Compute ns: count * 1e9 / freq, avoiding 64-bit overflow */
    return (uint64_t)((double)count.QuadPart * 1e9 / (double)freq.QuadPart);
}

static inline double bench_tsc_ghz(void) { return 0.0; }

#elif defined(__linux__) || defined(__APPLE__)

#  include <time.h>

static inline void bench_timer_init(void) {}

static inline uint64_t bench_now_ns(void) {
    struct timespec ts;
#  if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#  else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#  endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline double bench_tsc_ghz(void) { return 0.0; }

#else

/* Generic fallback using standard C clock() — low resolution, only for porting */
#  include <time.h>
static inline void bench_timer_init(void) {}
static inline uint64_t bench_now_ns(void) {
    return (uint64_t)((double)clock() / CLOCKS_PER_SEC * 1e9);
}
static inline double bench_tsc_ghz(void) { return 0.0; }

#endif /* platform */

#ifdef __cplusplus
}
#endif

#endif /* BENCH_TIMER_H */
