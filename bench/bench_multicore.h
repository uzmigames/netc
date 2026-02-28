/**
 * bench_multicore.h — Multi-core scaling benchmark (RFC-002 §5, task 4.4).
 *
 * Measures how compression throughput scales from 1 to N threads.
 * Each thread has its own independent corpus generator and compressor context.
 * The compressor factory function is called once per thread.
 *
 * Thread counts tested: 1, 2, 4, 8, 16.
 * Scaling efficiency = throughput(N) / (N * throughput(1)).
 *
 * Requires pthreads on Linux/macOS, Win32 threads on Windows.
 */

#ifndef BENCH_MULTICORE_H
#define BENCH_MULTICORE_H

#include "bench_compressor.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Multi-core result for one thread count
 * ========================================================================= */

typedef struct {
    int      nthreads;
    uint64_t packets;
    uint64_t original_bytes;
    double   compress_mbs;      /* aggregate MB/s across all threads */
    double   compress_mpps;     /* aggregate Mpps across all threads */
    double   scaling_efficiency; /* compress_mbs / (nthreads * single_thread_mbs) */
} bench_scaling_point_t;

/* =========================================================================
 * Full scaling report
 * ========================================================================= */

#define BENCH_SCALING_MAX_POINTS 8

typedef struct {
    const char            *compressor;
    bench_workload_t       workload;
    bench_scaling_point_t  points[BENCH_SCALING_MAX_POINTS];
    int                    n_points;
} bench_scaling_report_t;

/* =========================================================================
 * Factory callback — create one compressor instance per thread
 * ========================================================================= */

/**
 * A factory function that creates a fresh, trained bench_compressor_t.
 * Called once per thread. The returned adapter will be destroyed after the run.
 *
 * @param wl    workload to train on
 * @param seed  corpus seed for training
 * @param n     number of training packets
 * @param ctx   user-supplied context pointer (passed through from bench_multicore_run)
 */
typedef bench_compressor_t *(*bench_compressor_factory_t)(
    bench_workload_t wl, uint64_t seed, size_t n, void *ctx);

/* =========================================================================
 * Configuration
 * ========================================================================= */

typedef struct {
    size_t    warmup;          /* per-thread warmup packets */
    size_t    count;           /* per-thread measurement packets */
    uint64_t  seed;
    size_t    train_n;         /* training packets per thread */
    int       thread_counts[BENCH_SCALING_MAX_POINTS]; /* e.g. {1,2,4,8,16} */
    int       n_thread_counts;
} bench_multicore_cfg_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Run multi-core scaling benchmark.
 *
 * For each thread count in cfg->thread_counts, spawns N threads, each
 * compressing cfg->count packets using an independent compressor instance
 * created by factory(). Reports aggregate throughput and scaling efficiency.
 *
 * Returns 0 on success, -1 on error.
 */
int bench_multicore_run(const bench_multicore_cfg_t  *cfg,
                        bench_workload_t               wl,
                        bench_compressor_factory_t     factory,
                        void                          *factory_ctx,
                        bench_scaling_report_t        *out);

/** Print a scaling report to stdout. */
void bench_scaling_report_print(const bench_scaling_report_t *r);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_MULTICORE_H */
