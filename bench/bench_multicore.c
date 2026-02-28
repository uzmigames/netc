/**
 * bench_multicore.c — Multi-core scaling benchmark (RFC-002 §5, task 4.4).
 *
 * Uses pthreads on Linux/macOS and Win32 threads on Windows.
 * Each thread independently creates, trains, and runs its compressor.
 */

#include "bench_multicore.h"
#include "bench_corpus.h"
#include "bench_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Thread abstraction
 * ========================================================================= */

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HANDLE bench_thread_t;
typedef DWORD  bench_thread_ret_t;
#  define BENCH_THREAD_CALL WINAPI
static int bench_thread_create(bench_thread_t *t, bench_thread_ret_t (WINAPI *fn)(void*), void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}
static void bench_thread_join(bench_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#  include <pthread.h>
typedef pthread_t     bench_thread_t;
typedef void         *bench_thread_ret_t;
#  define BENCH_THREAD_CALL
static int bench_thread_create(bench_thread_t *t, void *(*fn)(void*), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static void bench_thread_join(bench_thread_t t) {
    pthread_join(t, NULL);
}
#endif

/* =========================================================================
 * Per-thread work item
 * ========================================================================= */

#define SCRATCH_CAP  (BENCH_CORPUS_MAX_PKT * 2u + 256u)

typedef struct {
    /* Input */
    bench_compressor_factory_t factory;
    void                      *factory_ctx;
    bench_workload_t           wl;
    uint64_t                   seed;
    size_t                     train_n;
    size_t                     warmup;
    size_t                     count;
    int                        thread_id;

    /* Output */
    uint64_t  packets;
    uint64_t  original_bytes;
    double    compress_elapsed_s;
} bench_thread_work_t;

static bench_thread_ret_t BENCH_THREAD_CALL thread_fn(void *arg)
{
    bench_thread_work_t *w = (bench_thread_work_t *)arg;

    /* Create and train compressor */
    bench_compressor_t *c = w->factory(w->wl,
                                       w->seed + (uint64_t)w->thread_id * 1000003ULL,
                                       w->train_n,
                                       w->factory_ctx);
    if (!c) return (bench_thread_ret_t)0;

    bench_corpus_t corpus;
    bench_corpus_init(&corpus, w->wl, w->seed ^ (uint64_t)w->thread_id);

    uint8_t cmp_buf[SCRATCH_CAP];

    /* Warm-up */
    for (size_t i = 0; i < w->warmup; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) break;
        c->compress(c, corpus.packet, plen, cmp_buf, sizeof(cmp_buf));
    }

    /* Measurement */
    uint64_t total_orig = 0;
    uint64_t packets    = 0;

    uint64_t t0 = bench_now_ns();
    for (size_t i = 0; i < w->count; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) { bench_corpus_reset(&corpus); plen = bench_corpus_next(&corpus); }
        c->compress(c, corpus.packet, plen, cmp_buf, sizeof(cmp_buf));
        total_orig += plen;
        packets++;
    }
    uint64_t t1 = bench_now_ns();

    w->packets          = packets;
    w->original_bytes   = total_orig;
    w->compress_elapsed_s = (double)(t1 - t0) * 1e-9;

    c->destroy(c);
    return (bench_thread_ret_t)0;
}

/* =========================================================================
 * Run one thread-count tier
 * ========================================================================= */

static int run_tier(const bench_multicore_cfg_t *cfg,
                    bench_workload_t              wl,
                    bench_compressor_factory_t    factory,
                    void                         *factory_ctx,
                    int                           nthreads,
                    bench_scaling_point_t        *pt)
{
    bench_thread_work_t *work = (bench_thread_work_t *)calloc(
        (size_t)nthreads, sizeof(bench_thread_work_t));
    bench_thread_t *threads = (bench_thread_t *)calloc(
        (size_t)nthreads, sizeof(bench_thread_t));

    if (!work || !threads) { free(work); free(threads); return -1; }

    for (int i = 0; i < nthreads; i++) {
        work[i].factory     = factory;
        work[i].factory_ctx = factory_ctx;
        work[i].wl          = wl;
        work[i].seed        = cfg->seed;
        work[i].train_n     = cfg->train_n;
        work[i].warmup      = cfg->warmup;
        work[i].count       = cfg->count;
        work[i].thread_id   = i;
    }

    /* Launch threads */
    for (int i = 0; i < nthreads; i++) {
        if (bench_thread_create(&threads[i], thread_fn, &work[i]) != 0) {
            /* Join already-started threads */
            for (int j = 0; j < i; j++) bench_thread_join(threads[j]);
            free(work); free(threads);
            return -1;
        }
    }

    /* Wait for all threads */
    for (int i = 0; i < nthreads; i++) bench_thread_join(threads[i]);

    /* Aggregate results */
    uint64_t total_packets = 0;
    uint64_t total_bytes   = 0;
    double   max_elapsed   = 0.0;  /* wall time = slowest thread */

    for (int i = 0; i < nthreads; i++) {
        total_packets += work[i].packets;
        total_bytes   += work[i].original_bytes;
        if (work[i].compress_elapsed_s > max_elapsed)
            max_elapsed = work[i].compress_elapsed_s;
    }

    free(work); free(threads);

    pt->nthreads       = nthreads;
    pt->packets        = total_packets;
    pt->original_bytes = total_bytes;
    pt->compress_mbs   = max_elapsed > 0
        ? (double)total_bytes / (1024.0 * 1024.0) / max_elapsed : 0.0;
    pt->compress_mpps  = max_elapsed > 0
        ? (double)total_packets / 1e6 / max_elapsed : 0.0;
    pt->scaling_efficiency = 0.0;  /* filled after all tiers */

    return 0;
}

/* =========================================================================
 * Public: bench_multicore_run
 * ========================================================================= */

int bench_multicore_run(const bench_multicore_cfg_t  *cfg,
                        bench_workload_t               wl,
                        bench_compressor_factory_t     factory,
                        void                          *factory_ctx,
                        bench_scaling_report_t        *out)
{
    bench_timer_init();

    memset(out, 0, sizeof(*out));
    out->workload = wl;

    int n = cfg->n_thread_counts;
    if (n > BENCH_SCALING_MAX_POINTS) n = BENCH_SCALING_MAX_POINTS;

    for (int i = 0; i < n; i++) {
        int nthreads = cfg->thread_counts[i];
        if (nthreads <= 0) nthreads = 1;
        int rc = run_tier(cfg, wl, factory, factory_ctx,
                          nthreads, &out->points[out->n_points]);
        if (rc == 0) out->n_points++;
    }

    /* Compute scaling efficiency relative to single-thread */
    double single_mbs = 0.0;
    for (int i = 0; i < out->n_points; i++) {
        if (out->points[i].nthreads == 1) {
            single_mbs = out->points[i].compress_mbs;
            break;
        }
    }
    if (single_mbs > 0.0) {
        for (int i = 0; i < out->n_points; i++) {
            int n_t = out->points[i].nthreads;
            out->points[i].scaling_efficiency =
                out->points[i].compress_mbs / ((double)n_t * single_mbs);
        }
    }

    return (out->n_points > 0) ? 0 : -1;
}

/* =========================================================================
 * Public: bench_scaling_report_print
 * ========================================================================= */

void bench_scaling_report_print(const bench_scaling_report_t *r)
{
    printf("Multi-core scaling — %s\n", r->compressor ? r->compressor : "?");
    printf("  %-8s  %-12s  %-12s  %s\n",
           "Threads", "Compress MB/s", "Mpps", "Efficiency");
    printf("  %-8s  %-12s  %-12s  %s\n",
           "-------", "-------------", "----", "----------");
    for (int i = 0; i < r->n_points; i++) {
        const bench_scaling_point_t *p = &r->points[i];
        printf("  %-8d  %-12.1f  %-12.3f  %.1f%%\n",
               p->nthreads,
               p->compress_mbs,
               p->compress_mpps,
               p->scaling_efficiency * 100.0);
    }
}
