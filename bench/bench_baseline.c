/**
 * bench_baseline.c — Baseline save/load and regression check (RFC-002 §6.4-6.5).
 *
 * Minimal JSON writer/reader using only libc (no external JSON library).
 * The format is a flat JSON object with numeric fields only.
 */

#include "bench_baseline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Filename helper
 * ========================================================================= */

void bench_baseline_filename(const char *dir,
                              const char *compressor,
                              const char *workload,
                              char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/%s_%s.json", dir, compressor, workload);
}

/* =========================================================================
 * bench_baseline_save
 * ========================================================================= */

int bench_baseline_save(const char *dir, const bench_result_t *result)
{
    if (!dir || !result) return -1;

    /* Workload name */
    const char *wl_name = "WL-???";
    switch (result->workload) {
        case BENCH_WL_001: wl_name = "WL-001"; break;
        case BENCH_WL_002: wl_name = "WL-002"; break;
        case BENCH_WL_003: wl_name = "WL-003"; break;
        case BENCH_WL_004: wl_name = "WL-004"; break;
        case BENCH_WL_005: wl_name = "WL-005"; break;
        case BENCH_WL_006: wl_name = "WL-006"; break;
        case BENCH_WL_007: wl_name = "WL-007"; break;
        case BENCH_WL_008: wl_name = "WL-008"; break;
        default: break;
    }

    char path[512];
    bench_baseline_filename(dir, result->compressor, wl_name, path, sizeof(path));

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp,
        "{\n"
        "  \"version\": 1,\n"
        "  \"compressor\": \"%s\",\n"
        "  \"workload\": \"%s\",\n"
        "  \"compress_mbs\": %.6f,\n"
        "  \"decompress_mbs\": %.6f,\n"
        "  \"ratio\": %.6f,\n"
        "  \"compress_p50_ns\": %.1f,\n"
        "  \"decompress_p50_ns\": %.1f\n"
        "}\n",
        result->compressor,
        wl_name,
        result->compress_mbs,
        result->decompress_mbs,
        result->ratio,
        (double)result->compress.p50_ns,
        (double)result->decompress.p50_ns);

    fclose(fp);
    return 0;
}

/* =========================================================================
 * Minimal JSON field reader
 * Reads "key": <number> from a flat JSON object.
 * Returns 1 if found, 0 if not found.
 * ========================================================================= */

static int json_read_double(const char *json, const char *key, double *out)
{
    /* Search for "key": */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    char *endptr;
    double v = strtod(p, &endptr);
    if (endptr == p) return 0;
    *out = v;
    return 1;
}

static int json_read_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* =========================================================================
 * bench_baseline_load
 * ========================================================================= */

int bench_baseline_load(const char *dir,
                        const char *compressor,
                        const char *workload,
                        bench_baseline_t *out)
{
    if (!dir || !compressor || !workload || !out) return -1;

    char path[512];
    bench_baseline_filename(dir, compressor, workload, path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    /* Read entire file into buffer */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 65536) { fclose(fp); return -1; }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* Parse fields */
    memset(out, 0, sizeof(*out));

    if (!json_read_string(buf, "compressor", out->compressor, sizeof(out->compressor)) ||
        !json_read_string(buf, "workload",   out->workload,   sizeof(out->workload)))
    {
        free(buf); return -1;
    }

    double v = 0.0;
    if (json_read_double(buf, "compress_mbs",      &v)) out->compress_mbs      = v;
    if (json_read_double(buf, "decompress_mbs",    &v)) out->decompress_mbs    = v;
    if (json_read_double(buf, "ratio",             &v)) out->ratio             = v;
    if (json_read_double(buf, "compress_p50_ns",   &v)) out->compress_p50_ns   = v;
    if (json_read_double(buf, "decompress_p50_ns", &v)) out->decompress_p50_ns = v;

    free(buf);
    return 0;
}

/* =========================================================================
 * bench_baseline_check
 * ========================================================================= */

static bench_reg_status_t check_metric(const char *name,
                                        double baseline,
                                        double actual,
                                        double *pct_out,
                                        bench_reg_check_t *chk)
{
    chk->metric   = name;
    chk->baseline = baseline;
    chk->actual   = actual;

    if (baseline == 0.0) {
        chk->pct_change = 0.0;
        chk->status     = BENCH_REG_OK;
        if (pct_out) *pct_out = 0.0;
        return BENCH_REG_OK;
    }

    double pct = (actual - baseline) / baseline * 100.0;
    chk->pct_change = pct;
    if (pct_out) *pct_out = pct;

    double abs_pct = pct < 0 ? -pct : pct;
    if (abs_pct >= 15.0) {
        chk->status = BENCH_REG_FAIL;
    } else if (abs_pct >= 5.0) {
        chk->status = BENCH_REG_WARN;
    } else {
        chk->status = BENCH_REG_OK;
    }
    return chk->status;
}

int bench_baseline_check(const bench_baseline_t *baseline,
                          const bench_result_t   *result,
                          bench_reg_report_t     *report)
{
    memset(report, 0, sizeof(*report));
    strncpy(report->compressor, baseline->compressor, sizeof(report->compressor) - 1);
    strncpy(report->workload,   baseline->workload,   sizeof(report->workload)   - 1);

    double pct;

    /* compress_mbs — higher is better; regression = decrease */
    check_metric("compress_mbs", baseline->compress_mbs, result->compress_mbs,
                 &pct, &report->checks[report->n_checks++]);

    /* decompress_mbs */
    check_metric("decompress_mbs", baseline->decompress_mbs, result->decompress_mbs,
                 &pct, &report->checks[report->n_checks++]);

    /* ratio — lower is better (smaller compressed output);
     * an increase means compression got worse, treat same thresholds */
    check_metric("ratio", baseline->ratio, result->ratio,
                 &pct, &report->checks[report->n_checks++]);

    /* compress latency p50 — lower is better; increase is regression */
    double actual_p50c = (double)result->compress.p50_ns;
    check_metric("compress_p50_ns", baseline->compress_p50_ns, actual_p50c,
                 &pct, &report->checks[report->n_checks++]);

    /* decompress latency p50 */
    double actual_p50d = (double)result->decompress.p50_ns;
    check_metric("decompress_p50_ns", baseline->decompress_p50_ns, actual_p50d,
                 &pct, &report->checks[report->n_checks++]);

    /* Aggregate */
    for (int i = 0; i < report->n_checks; i++) {
        if (report->checks[i].status == BENCH_REG_FAIL) report->any_fail = 1;
        if (report->checks[i].status == BENCH_REG_WARN) report->any_warn = 1;
    }

    return report->any_fail ? 1 : 0;
}

/* =========================================================================
 * bench_reg_report_print
 * ========================================================================= */

void bench_reg_report_print(const bench_reg_report_t *r)
{
    static const char *status_str[] = { "OK  ", "WARN", "FAIL" };
    printf("Regression check: %s / %s\n", r->compressor, r->workload);
    printf("  %-22s  %-12s  %-12s  %8s  %s\n",
           "Metric", "Baseline", "Actual", "Change", "Status");
    printf("  %-22s  %-12s  %-12s  %8s  %s\n",
           "------", "--------", "------", "------", "------");
    for (int i = 0; i < r->n_checks; i++) {
        const bench_reg_check_t *c = &r->checks[i];
        printf("  %-22s  %-12.4g  %-12.4g  %+7.1f%%  %s\n",
               c->metric,
               c->baseline,
               c->actual,
               c->pct_change,
               status_str[c->status < 3 ? c->status : 0]);
    }
    printf("  Result: %s\n",
           r->any_fail ? "FAIL" : r->any_warn ? "WARN" : "PASS");
}
