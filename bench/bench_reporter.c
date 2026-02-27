/**
 * bench_reporter.c — Table, CSV, and JSON output.
 */

#include "bench_reporter.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Internal state
 * ========================================================================= */
struct bench_reporter {
    bench_format_t fmt;
    FILE          *fp;
    int            first_row;   /* JSON needs comma logic */
};

/* =========================================================================
 * Helpers
 * ========================================================================= */
static void fill_derived(bench_result_t *r)
{
    if (r->compress_mbs == 0.0 && r->pkt_size > 0)
        r->compress_mbs = bench_stats_throughput_mbs(r->pkt_size, r->compress.mean_ns);
    if (r->compress_mpps == 0.0)
        r->compress_mpps = bench_stats_mpps(r->compress.mean_ns);
    if (r->decompress_mbs == 0.0 && r->pkt_size > 0)
        r->decompress_mbs = bench_stats_throughput_mbs(r->pkt_size, r->decompress.mean_ns);
    if (r->decompress_mpps == 0.0)
        r->decompress_mpps = bench_stats_mpps(r->decompress.mean_ns);
    if (r->ratio == 0.0 && r->original_bytes > 0)
        r->ratio = bench_stats_ratio(r->original_bytes, r->compressed_bytes);
}

static void iso8601_now(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm_info;
#if defined(_WIN32)
    struct tm tm_buf;
    gmtime_s(&tm_buf, &t);
    tm_info = &tm_buf;
#else
    tm_info = gmtime(&t);
#endif
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

/* =========================================================================
 * Open / close
 * ========================================================================= */
bench_reporter_t *bench_reporter_open(bench_format_t fmt, FILE *fp)
{
    bench_reporter_t *r = (bench_reporter_t *)malloc(sizeof(*r));
    if (!r) return NULL;
    r->fmt       = fmt;
    r->fp        = fp ? fp : stdout;
    r->first_row = 1;
    return r;
}

void bench_reporter_close(bench_reporter_t *r)
{
    free(r);
}

/* =========================================================================
 * Begin / end
 * ========================================================================= */
void bench_reporter_begin(bench_reporter_t *r,
                          const char *version,
                          const char *cpu_desc)
{
    if (!r) return;
    switch (r->fmt) {
    case BENCH_FMT_TABLE:
        fprintf(r->fp,
            "%-12s %-24s %-8s %9s %9s %9s %9s %9s %9s %8s %8s\n",
            "Compressor", "Workload", "Size",
            "c.p50ns", "c.p99ns", "d.p50ns", "d.p99ns",
            "c.MB/s", "d.MB/s", "Ratio", "Mpps-c");
        fprintf(r->fp, "%s\n",
            "-------------------------------------------------------------"
            "------------------------------------------------------------------");
        break;
    case BENCH_FMT_CSV:
        fprintf(r->fp,
            "date,version,compressor,compressor_cfg,workload,packet_size,"
            "count,seed,"
            "compress_p50_ns,compress_p90_ns,compress_p99_ns,compress_p999_ns,"
            "compress_mean_ns,compress_stddev_ns,compress_mbs,compress_mpps,"
            "decompress_p50_ns,decompress_p90_ns,decompress_p99_ns,decompress_p999_ns,"
            "decompress_mean_ns,decompress_stddev_ns,decompress_mbs,decompress_mpps,"
            "ratio,original_bytes,compressed_bytes\n");
        (void)version; (void)cpu_desc;
        break;
    case BENCH_FMT_JSON: {
        char ts[32];
        iso8601_now(ts, sizeof(ts));
        fprintf(r->fp, "{\n");
        fprintf(r->fp, "  \"date\": \"%s\",\n", ts);
        fprintf(r->fp, "  \"version\": \"%s\",\n", version ? version : "unknown");
        fprintf(r->fp, "  \"system\": { \"cpu\": \"%s\" },\n",
                cpu_desc ? cpu_desc : "");
        fprintf(r->fp, "  \"results\": [\n");
        r->first_row = 1;
        break;
    }
    }
}

void bench_reporter_end(bench_reporter_t *r)
{
    if (!r) return;
    if (r->fmt == BENCH_FMT_JSON) {
        fprintf(r->fp, "\n  ]\n}\n");
    }
    fflush(r->fp);
}

/* =========================================================================
 * Write one result
 * ========================================================================= */
void bench_reporter_write(bench_reporter_t *r, bench_result_t *res)
{
    if (!r || !res) return;
    fill_derived(res);

    const char *wl_name = bench_workload_name(res->workload);

    switch (r->fmt) {
    case BENCH_FMT_TABLE:
        fprintf(r->fp,
            "%-12s %-24s %8zu %9" PRIu64 " %9" PRIu64
            " %9" PRIu64 " %9" PRIu64
            " %9.1f %9.1f %8.4f %8.3f\n",
            res->compressor ? res->compressor : "?",
            wl_name,
            res->pkt_size,
            res->compress.p50_ns,
            res->compress.p99_ns,
            res->decompress.p50_ns,
            res->decompress.p99_ns,
            res->compress_mbs,
            res->decompress_mbs,
            res->ratio,
            res->compress_mpps);
        break;

    case BENCH_FMT_CSV: {
        char ts[32];
        iso8601_now(ts, sizeof(ts));
        fprintf(r->fp,
            "%s,unknown,%s,%s,%s,%zu,%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%.2f,%.2f,%.2f,%.4f,"
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%.2f,%.2f,%.2f,%.4f,"
            "%.6f,%" PRIu64 ",%" PRIu64 "\n",
            ts,
            res->compressor ? res->compressor : "",
            res->compressor_cfg ? res->compressor_cfg : "",
            wl_name,
            res->pkt_size,
            res->count,
            res->seed,
            res->compress.p50_ns,
            res->compress.p90_ns,
            res->compress.p99_ns,
            res->compress.p999_ns,
            res->compress.mean_ns,
            res->compress.stddev_ns,
            res->compress_mbs,
            res->compress_mpps,
            res->decompress.p50_ns,
            res->decompress.p90_ns,
            res->decompress.p99_ns,
            res->decompress.p999_ns,
            res->decompress.mean_ns,
            res->decompress.stddev_ns,
            res->decompress_mbs,
            res->decompress_mpps,
            res->ratio,
            res->original_bytes,
            res->compressed_bytes);
        break;
    }

    case BENCH_FMT_JSON:
        if (!r->first_row) fprintf(r->fp, ",\n");
        r->first_row = 0;
        fprintf(r->fp,
            "    {\n"
            "      \"compressor\": \"%s\",\n"
            "      \"compressor_cfg\": \"%s\",\n"
            "      \"workload\": \"%s\",\n"
            "      \"packet_size\": %zu,\n"
            "      \"count\": %" PRIu64 ",\n"
            "      \"seed\": %" PRIu64 ",\n"
            "      \"compress\": {\n"
            "        \"p50_ns\": %" PRIu64 ", \"p90_ns\": %" PRIu64 ",\n"
            "        \"p99_ns\": %" PRIu64 ", \"p999_ns\": %" PRIu64 ",\n"
            "        \"mean_ns\": %.2f, \"stddev_ns\": %.2f,\n"
            "        \"throughput_mbs\": %.1f, \"mpps\": %.4f\n"
            "      },\n"
            "      \"decompress\": {\n"
            "        \"p50_ns\": %" PRIu64 ", \"p90_ns\": %" PRIu64 ",\n"
            "        \"p99_ns\": %" PRIu64 ", \"p999_ns\": %" PRIu64 ",\n"
            "        \"mean_ns\": %.2f, \"stddev_ns\": %.2f,\n"
            "        \"throughput_mbs\": %.1f, \"mpps\": %.4f\n"
            "      },\n"
            "      \"ratio\": %.6f,\n"
            "      \"original_bytes\": %" PRIu64 ",\n"
            "      \"compressed_bytes\": %" PRIu64 "\n"
            "    }",
            res->compressor ? res->compressor : "",
            res->compressor_cfg ? res->compressor_cfg : "",
            wl_name,
            res->pkt_size,
            res->count,
            res->seed,
            res->compress.p50_ns, res->compress.p90_ns,
            res->compress.p99_ns, res->compress.p999_ns,
            res->compress.mean_ns, res->compress.stddev_ns,
            res->compress_mbs, res->compress_mpps,
            res->decompress.p50_ns, res->decompress.p90_ns,
            res->decompress.p99_ns, res->decompress.p999_ns,
            res->decompress.mean_ns, res->decompress.stddev_ns,
            res->decompress_mbs, res->decompress_mpps,
            res->ratio,
            res->original_bytes,
            res->compressed_bytes);
        break;
    }
}

/* =========================================================================
 * Utility
 * ========================================================================= */
bench_format_t bench_format_parse(const char *s)
{
    if (!s) return BENCH_FMT_TABLE;
    if (strcmp(s, "csv")  == 0) return BENCH_FMT_CSV;
    if (strcmp(s, "json") == 0) return BENCH_FMT_JSON;
    return BENCH_FMT_TABLE;
}
