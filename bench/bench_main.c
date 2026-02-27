/**
 * bench_main.c — CLI entry point for the netc benchmark harness.
 *
 * Usage: bench [OPTIONS]
 *
 *   --workload=WL-001..008    Run specific workload(s) (default: all)
 *   --count=N                 Measurement iterations (default: 100000)
 *   --warmup=N                Warmup iterations (default: 1000)
 *   --seed=N                  PRNG seed (default: 42)
 *   --train=N                 Training corpus size (default: 50000)
 *   --format=table|csv|json   Output format (default: table)
 *   --output=FILE             Write output to FILE (default: stdout)
 *   --ci-check                Run CI gate checks and exit 0/1
 *   --no-dict                 Skip dictionary training (passthrough mode)
 *   --no-delta                Disable delta encoding
 *   --simd=auto|generic|sse42|avx2  Force SIMD level
 */

#include "bench_corpus.h"
#include "bench_netc.h"
#include "bench_runner.h"
#include "bench_reporter.h"
#include "../include/netc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Argument parsing
 * ========================================================================= */
typedef struct {
    /* Workload selection: bit N set → run workload N */
    uint32_t workload_mask;

    size_t   count;
    size_t   warmup;
    uint64_t seed;
    size_t   train_count;

    bench_format_t format;
    const char    *output_file;

    int ci_check;
    int no_dict;
    int no_delta;
    uint8_t simd_level;
} bench_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --workload=WL-NNN   Run workload(s); may repeat (e.g. --workload=WL-001 --workload=WL-002)\n"
        "  --count=N           Measurement iterations [default: %u]\n"
        "  --warmup=N          Warmup iterations [default: %u]\n"
        "  --seed=N            PRNG seed [default: %u]\n"
        "  --train=N           Training corpus size [default: %u]\n"
        "  --format=FMT        Output format: table|csv|json [default: table]\n"
        "  --output=FILE       Write results to FILE [default: stdout]\n"
        "  --ci-check          Run CI gates and exit 0=pass / 1=fail\n"
        "  --no-dict           Skip dictionary training\n"
        "  --no-delta          Disable delta encoding\n"
        "  --simd=LEVEL        Force SIMD: auto|generic|sse42|avx2 [default: auto]\n"
        "  --help              Show this help\n"
        "\n",
        prog,
        (unsigned)BENCH_DEFAULT_COUNT,
        (unsigned)BENCH_DEFAULT_WARMUP,
        (unsigned)BENCH_DEFAULT_SEED,
        (unsigned)BENCH_CORPUS_TRAIN_N);
}

static bench_workload_t parse_workload(const char *s)
{
    if (!s) return BENCH_WL_ALL;
    /* Accept "WL-001" or "1" or "001" */
    const char *p = s;
    if (strncmp(p, "WL-", 3) == 0) p += 3;
    int n = atoi(p);
    if (n >= 1 && n <= 8) return (bench_workload_t)n;
    return BENCH_WL_ALL;
}

static uint8_t parse_simd(const char *s)
{
    if (!s || strcmp(s, "auto")    == 0) return 0;
    if (       strcmp(s, "generic") == 0) return 1;
    if (       strcmp(s, "sse42")   == 0) return 2;
    if (       strcmp(s, "avx2")    == 0) return 3;
    return 0;
}

static int parse_args(int argc, char **argv, bench_args_t *a)
{
    memset(a, 0, sizeof(*a));
    a->count       = BENCH_DEFAULT_COUNT;
    a->warmup      = BENCH_DEFAULT_WARMUP;
    a->seed        = BENCH_DEFAULT_SEED;
    a->train_count = BENCH_CORPUS_TRAIN_N;
    a->workload_mask = 0;  /* 0 = all */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (strcmp(arg, "--ci-check") == 0) { a->ci_check = 1; continue; }
        if (strcmp(arg, "--no-dict")  == 0) { a->no_dict  = 1; continue; }
        if (strcmp(arg, "--no-delta") == 0) { a->no_delta = 1; continue; }

        /* Key=value arguments */
        const char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
        char key[64];
        size_t klen = (size_t)(eq - arg);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, arg, klen);
        key[klen] = '\0';
        const char *val = eq + 1;

        if (strcmp(key, "--workload") == 0) {
            bench_workload_t wl = parse_workload(val);
            if (wl == BENCH_WL_ALL) {
                fprintf(stderr, "Unknown workload: %s\n", val);
                return -1;
            }
            a->workload_mask |= (1u << (unsigned)wl);
        } else if (strcmp(key, "--count")  == 0) { a->count       = (size_t)atol(val); }
        else if   (strcmp(key, "--warmup") == 0) { a->warmup      = (size_t)atol(val); }
        else if   (strcmp(key, "--seed")   == 0) { a->seed        = (uint64_t)atoll(val); }
        else if   (strcmp(key, "--train")  == 0) { a->train_count = (size_t)atol(val); }
        else if   (strcmp(key, "--format") == 0) { a->format      = bench_format_parse(val); }
        else if   (strcmp(key, "--output") == 0) { a->output_file = val; }
        else if   (strcmp(key, "--simd")   == 0) { a->simd_level  = parse_simd(val); }
        else {
            fprintf(stderr, "Unknown option: %s\n", key);
            return -1;
        }
    }

    /* Default: run all workloads */
    if (a->workload_mask == 0) {
        for (int w = 1; w <= 8; w++) a->workload_mask |= (1u << (unsigned)w);
    }

    return 0;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char **argv)
{
    bench_args_t args;
    int rc = parse_args(argc, argv, &args);
    if (rc != 0) return (rc > 0) ? 0 : 2;

    /* Open output file */
    FILE *out_fp = stdout;
    if (args.output_file) {
        out_fp = fopen(args.output_file, "w");
        if (!out_fp) {
            fprintf(stderr, "Cannot open output file: %s\n", args.output_file);
            return 2;
        }
    }

    /* Reporter */
    bench_reporter_t *reporter = bench_reporter_open(args.format, out_fp);
    if (!reporter) { fprintf(stderr, "OOM\n"); return 2; }

    bench_reporter_begin(reporter, NETC_VERSION_STR, "");

    /* SIMD level and flags */
    uint32_t flags = NETC_CFG_FLAG_STATEFUL;
    if (!args.no_delta) flags |= NETC_CFG_FLAG_DELTA;

    /* Results accumulator for CI gates */
    bench_result_t results[8];
    int n_results = 0;

    /* Run each selected workload */
    for (int wl_id = 1; wl_id <= 8; wl_id++) {
        if (!(args.workload_mask & (1u << (unsigned)wl_id))) continue;

        bench_workload_t wl = (bench_workload_t)wl_id;
        fprintf(stderr, "Running %s ...\n", bench_workload_name(wl));

        /* Initialize netc adapter */
        bench_netc_t netc;
        if (bench_netc_init(&netc, NULL, flags, args.simd_level,
                            BENCH_CORPUS_MAX_PKT) != 0)
        {
            fprintf(stderr, "Failed to init netc adapter for %s\n",
                    bench_workload_name(wl));
            continue;
        }

        /* Train dictionary unless disabled */
        if (!args.no_dict) {
            fprintf(stderr, "  Training dictionary (%zu packets) ...\n",
                    args.train_count);
            if (bench_netc_train(&netc, wl, args.seed, args.train_count) != 0) {
                fprintf(stderr, "  Dictionary training failed — running without dict\n");
            }
        }

        /* Run benchmark */
        bench_run_cfg_t run_cfg = {
            .warmup = args.warmup,
            .count  = args.count,
            .seed   = args.seed,
        };

        bench_result_t result;
        memset(&result, 0, sizeof(result));

        if (bench_run(&run_cfg, wl, &netc, &result) == 0) {
            bench_reporter_write(reporter, &result);

            if (n_results < 8) {
                results[n_results++] = result;
            }
        } else {
            fprintf(stderr, "  Benchmark FAILED for %s\n", bench_workload_name(wl));
        }

        bench_netc_destroy(&netc);
    }

    bench_reporter_end(reporter);
    bench_reporter_close(reporter);

    if (out_fp != stdout) fclose(out_fp);

    /* CI gate check */
    if (args.ci_check) {
        bench_ci_report_t ci;
        int ci_rc = bench_ci_check(results, (size_t)n_results, &ci);
        bench_ci_report_print(&ci);
        return ci_rc;
    }

    return 0;
}
