/**
 * bench_baseline.h — Baseline save/load and regression check (RFC-002 §6.4-6.5).
 *
 * Baseline files are stored as JSON in bench/baselines/<name>.json.
 * The JSON schema matches the bench_result_t fields needed for regression
 * comparison:
 *
 *   {
 *     "version": 1,
 *     "compressor": "netc",
 *     "workload": "WL-001",
 *     "compress_mbs": 1234.5,
 *     "decompress_mbs": 2345.6,
 *     "ratio": 0.42,
 *     "compress_p50_ns": 120.0,
 *     "decompress_p50_ns": 60.0
 *   }
 *
 * Regression gates (RFC-002 §6.4):
 *   - ±5%  → WARNING  (printed but does not fail)
 *   - ±15% → FAIL     (returns non-zero)
 */

#ifndef BENCH_BASELINE_H
#define BENCH_BASELINE_H

#include "bench_reporter.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * One baseline entry
 * ========================================================================= */

typedef struct {
    char     compressor[64];
    char     workload[16];       /* "WL-001" etc. */

    double   compress_mbs;
    double   decompress_mbs;
    double   ratio;
    double   compress_p50_ns;
    double   decompress_p50_ns;
} bench_baseline_t;

/* =========================================================================
 * Regression result for one metric
 * ========================================================================= */

typedef enum {
    BENCH_REG_OK      = 0,   /* within ±5% */
    BENCH_REG_WARN    = 1,   /* ±5% – ±15% */
    BENCH_REG_FAIL    = 2,   /* outside ±15% */
} bench_reg_status_t;

typedef struct {
    const char        *metric;
    double             baseline;
    double             actual;
    double             pct_change;  /* (actual - baseline) / baseline * 100 */
    bench_reg_status_t status;
} bench_reg_check_t;

#define BENCH_MAX_REG_CHECKS 8

typedef struct {
    char              compressor[64];
    char              workload[16];
    bench_reg_check_t checks[BENCH_MAX_REG_CHECKS];
    int               n_checks;
    int               any_fail;
    int               any_warn;
} bench_reg_report_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Save a benchmark result as a baseline JSON file.
 *
 * @param dir      Directory to write into (e.g. "bench/baselines")
 * @param result   The result to save
 * Returns 0 on success, -1 on I/O error.
 */
int bench_baseline_save(const char *dir, const bench_result_t *result);

/**
 * Load a baseline from a JSON file.
 *
 * @param dir         Directory to look in (e.g. "bench/baselines")
 * @param compressor  Compressor name (e.g. "netc")
 * @param workload    Workload name (e.g. "WL-001")
 * @param out         Receives the loaded baseline
 * Returns 0 on success, -1 if file not found or parse error.
 */
int bench_baseline_load(const char *dir,
                        const char *compressor,
                        const char *workload,
                        bench_baseline_t *out);

/**
 * Compare a result against a stored baseline.
 *
 * Checks compress_mbs, decompress_mbs, and ratio.
 * Fills *report with per-metric pass/warn/fail.
 *
 * Returns 0 if all checks pass, 1 if any check fails.
 */
int bench_baseline_check(const bench_baseline_t *baseline,
                         const bench_result_t   *result,
                         bench_reg_report_t     *report);

/** Print a regression report to stdout. */
void bench_reg_report_print(const bench_reg_report_t *r);

/**
 * Compute the baseline filename for (dir, compressor, workload).
 * Writes to buf (size buf_size).
 */
void bench_baseline_filename(const char *dir,
                             const char *compressor,
                             const char *workload,
                             char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_BASELINE_H */
