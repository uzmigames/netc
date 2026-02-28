/**
 * bench_runner.h — Benchmark execution engine: warmup + timing loop + CI gates.
 *
 * Per RFC-002 §5:
 *   - 1,000 warmup iterations (not timed)
 *   - 100,000 measurement iterations, each individually timed
 *   - p50, p90, p99, p999 computed from sorted samples
 *   - CI gate checker (--ci-check): enforces PERF-*, RATIO-*, SAFETY-*, MEM-* gates
 *
 * Usage:
 *   bench_run_cfg_t cfg = { .warmup = 1000, .count = 100000, .seed = 42 };
 *   bench_result_t result;
 *   bench_run(&cfg, workload, &netc_adapter, &result);
 */

#ifndef BENCH_RUNNER_H
#define BENCH_RUNNER_H

#include "bench_corpus.h"
#include "bench_netc.h"
#include "bench_reporter.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Run configuration
 * ========================================================================= */
typedef struct {
    size_t   warmup;     /* warmup iterations (RFC-002: 1,000) */
    size_t   count;      /* measurement iterations (RFC-002: 100,000) */
    uint64_t seed;       /* corpus PRNG seed */
} bench_run_cfg_t;

/* Default values per RFC-002 §5 */
#define BENCH_DEFAULT_WARMUP  1000u
#define BENCH_DEFAULT_COUNT   100000u
#define BENCH_DEFAULT_SEED    42u

/* Evaluation seed offset: test packets come from seed + OFFSET so they are
 * from the same distribution but unseen during training.  This prevents
 * dictionary-based compressors (e.g. OodleNetwork) from getting an unfair
 * advantage by hash-matching raw training bytes in their window. */
#define BENCH_EVAL_SEED_OFFSET  0x1000001u

/* =========================================================================
 * Run one benchmark: compress + decompress latency for a workload
 * ========================================================================= */

/**
 * Run a single latency benchmark.
 *
 * Generates `cfg->warmup + cfg->count` packets from `wl`.
 * First `warmup` packets are not timed (cache warmup).
 * Remaining `count` packets are timed individually.
 *
 * Both compress and decompress are timed end-to-end.
 * The decompressed output is verified against the original (SAFETY-01).
 *
 * Returns 0 on success, -1 on error.
 * Writes timing stats and ratio into *out.
 */
int bench_run(const bench_run_cfg_t *cfg,
              bench_workload_t       wl,
              bench_netc_t          *netc,
              bench_result_t        *out);

/* =========================================================================
 * CI gate checker
 * ========================================================================= */

/**
 * Gate result: PASS or FAIL with a description.
 */
typedef struct {
    const char *gate_id;     /* e.g. "PERF-01" */
    const char *description; /* human-readable criterion */
    double      actual;      /* measured value */
    double      threshold;   /* required value */
    int         passed;      /* 1 = pass, 0 = fail */
} bench_gate_result_t;

#define BENCH_MAX_GATES 32

typedef struct {
    bench_gate_result_t gates[BENCH_MAX_GATES];
    int                 n_gates;
    int                 all_passed;
} bench_ci_report_t;

/**
 * Check all RFC-002 §6.1 absolute performance gates against a result set.
 *
 * results: array of bench_result_t (one per workload)
 * n:       number of results
 * report:  receives gate pass/fail details
 *
 * Returns 0 if all gates pass, 1 if any gate fails.
 */
int bench_ci_check(const bench_result_t *results, size_t n,
                   bench_ci_report_t    *report);

/** Print a CI report to stdout. */
void bench_ci_report_print(const bench_ci_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_RUNNER_H */
