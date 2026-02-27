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
#include "bench_compressor.h"
#include "bench_zlib.h"
#include "bench_lz4.h"
#include "bench_zstd.h"
#include "../include/netc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Argument parsing
 * ========================================================================= */
/* Bit flags for --compressor selection */
#define BENCH_COMP_NETC     (1u << 0)
#define BENCH_COMP_ZLIB1    (1u << 1)
#define BENCH_COMP_ZLIB6    (1u << 2)
#define BENCH_COMP_LZ4      (1u << 3)
#define BENCH_COMP_LZ4HC    (1u << 4)
#define BENCH_COMP_ZSTD1    (1u << 5)
#define BENCH_COMP_ZSTD3    (1u << 6)
#define BENCH_COMP_ZSTD1D   (1u << 7)   /* Zstd level=1 + dict */
#define BENCH_COMP_ALL      (0xFFu)

typedef struct {
    /* Workload selection: bit N set → run workload N */
    uint32_t workload_mask;
    /* Compressor selection: BENCH_COMP_* bitmask */
    uint32_t compressor_mask;

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
        "  --workload=WL-NNN   Run workload(s); may repeat (default: all)\n"
        "  --compressor=NAME   Run compressor(s): netc|zlib-1|zlib-6|lz4|lz4-hc|\n"
        "                        zstd-1|zstd-3|zstd-1-dict|all (default: netc)\n"
        "  --count=N           Measurement iterations [default: %u]\n"
        "  --warmup=N          Warmup iterations [default: %u]\n"
        "  --seed=N            PRNG seed [default: %u]\n"
        "  --train=N           Training corpus size [default: %u]\n"
        "  --format=FMT        Output format: table|csv|json [default: table]\n"
        "  --output=FILE       Write results to FILE [default: stdout]\n"
        "  --ci-check          Run CI gates and exit 0=pass / 1=fail\n"
        "  --no-dict           Skip dictionary training (netc only)\n"
        "  --no-delta          Disable delta encoding (netc only)\n"
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

static uint32_t parse_compressor(const char *s)
{
    if (!s || strcmp(s, "all")        == 0) return BENCH_COMP_ALL;
    if (       strcmp(s, "netc")      == 0) return BENCH_COMP_NETC;
    if (       strcmp(s, "zlib-1")    == 0) return BENCH_COMP_ZLIB1;
    if (       strcmp(s, "zlib-6")    == 0) return BENCH_COMP_ZLIB6;
    if (       strcmp(s, "lz4")       == 0) return BENCH_COMP_LZ4;
    if (       strcmp(s, "lz4-hc")    == 0) return BENCH_COMP_LZ4HC;
    if (       strcmp(s, "zstd-1")    == 0) return BENCH_COMP_ZSTD1;
    if (       strcmp(s, "zstd-3")    == 0) return BENCH_COMP_ZSTD3;
    if (       strcmp(s, "zstd-1-dict") == 0) return BENCH_COMP_ZSTD1D;
    return 0; /* unknown */
}

static int parse_args(int argc, char **argv, bench_args_t *a)
{
    memset(a, 0, sizeof(*a));
    a->count          = BENCH_DEFAULT_COUNT;
    a->warmup         = BENCH_DEFAULT_WARMUP;
    a->seed           = BENCH_DEFAULT_SEED;
    a->train_count    = BENCH_CORPUS_TRAIN_N;
    a->workload_mask  = 0;   /* 0 = all */
    a->compressor_mask = 0;  /* 0 → default to netc only */

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
        } else if (strcmp(key, "--compressor") == 0) {
            uint32_t mask = parse_compressor(val);
            if (mask == 0) {
                fprintf(stderr, "Unknown compressor: %s\n", val);
                return -1;
            }
            a->compressor_mask |= mask;
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

    /* Defaults */
    if (a->workload_mask  == 0) {
        for (int w = 1; w <= 8; w++) a->workload_mask |= (1u << (unsigned)w);
    }
    if (a->compressor_mask == 0) a->compressor_mask = BENCH_COMP_NETC;

    return 0;
}

/* =========================================================================
 * Comparison result storage for COMP-* gates
 * Max: 8 workloads × 9 compressors = 72 results
 * ========================================================================= */
#define BENCH_MAX_RESULTS 128

/* =========================================================================
 * Run one bench_compressor_t over a workload and record result
 * ========================================================================= */
static void run_compressor(bench_compressor_t    *c,
                           bench_workload_t        wl,
                           const bench_args_t     *args,
                           bench_reporter_t       *reporter,
                           bench_result_t         *results,
                           int                    *n_results)
{
    bench_generic_cfg_t gcfg;
    gcfg.warmup = args->warmup;
    gcfg.count  = args->count;
    gcfg.seed   = args->seed;

    bench_result_t res;
    memset(&res, 0, sizeof(res));

    if (bench_run_generic(&gcfg, wl, c, &res) == 0) {
        bench_reporter_write(reporter, &res);
        if (*n_results < BENCH_MAX_RESULTS) {
            results[(*n_results)++] = res;
        }
    } else {
        fprintf(stderr, "  [%s] FAILED on %s\n", c->name, bench_workload_name(wl));
    }
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

    bench_reporter_t *reporter = bench_reporter_open(args.format, out_fp);
    if (!reporter) { fprintf(stderr, "OOM\n"); return 2; }
    bench_reporter_begin(reporter, NETC_VERSION_STR, "");

    uint32_t flags = NETC_CFG_FLAG_STATEFUL;
    if (!args.no_delta) flags |= NETC_CFG_FLAG_DELTA;

    /* Allocate result storage */
    bench_result_t *results = (bench_result_t *)calloc(BENCH_MAX_RESULTS,
                                                        sizeof(bench_result_t));
    if (!results) { fprintf(stderr, "OOM\n"); return 2; }
    int n_results = 0;

    /* Also keep the netc-only WL-001 result for COMP-* gates */
    bench_result_t netc_wl001;
    int            have_netc_wl001 = 0;

    for (int wl_id = 1; wl_id <= 8; wl_id++) {
        if (!(args.workload_mask & (1u << (unsigned)wl_id))) continue;
        bench_workload_t wl = (bench_workload_t)wl_id;
        fprintf(stderr, "=== %s ===\n", bench_workload_name(wl));

        /* --- netc --- */
        if (args.compressor_mask & BENCH_COMP_NETC) {
            bench_netc_t netc_adapter;
            if (bench_netc_init(&netc_adapter, NULL, flags, args.simd_level,
                                BENCH_CORPUS_MAX_PKT) == 0)
            {
                if (!args.no_dict) {
                    fprintf(stderr, "  [netc] Training dict (%zu pkts)...\n",
                            args.train_count);
                    bench_netc_train(&netc_adapter, wl, args.seed, args.train_count);
                }

                bench_run_cfg_t rcfg = { args.warmup, args.count, args.seed };
                bench_result_t res;
                memset(&res, 0, sizeof(res));
                if (bench_run(&rcfg, wl, &netc_adapter, &res) == 0) {
                    bench_reporter_write(reporter, &res);
                    if (n_results < BENCH_MAX_RESULTS)
                        results[n_results++] = res;
                    if (wl == BENCH_WL_001) {
                        netc_wl001     = res;
                        have_netc_wl001 = 1;
                    }
                } else {
                    fprintf(stderr, "  [netc] FAILED on %s\n", bench_workload_name(wl));
                }
                bench_netc_destroy(&netc_adapter);
            }
        }

        /* --- zlib level 1 --- */
        if (args.compressor_mask & BENCH_COMP_ZLIB1) {
            bench_compressor_t *c = bench_zlib_create(1);
            if (c) {
                fprintf(stderr, "  [zlib-1] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [zlib-1] not available\n"); }
        }

        /* --- zlib level 6 --- */
        if (args.compressor_mask & BENCH_COMP_ZLIB6) {
            bench_compressor_t *c = bench_zlib_create(6);
            if (c) {
                fprintf(stderr, "  [zlib-6] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [zlib-6] not available\n"); }
        }

        /* --- LZ4 fast --- */
        if (args.compressor_mask & BENCH_COMP_LZ4) {
            bench_compressor_t *c = bench_lz4_create(0);
            if (c) {
                fprintf(stderr, "  [lz4-fast] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [lz4-fast] not available\n"); }
        }

        /* --- LZ4 HC --- */
        if (args.compressor_mask & BENCH_COMP_LZ4HC) {
            bench_compressor_t *c = bench_lz4_create(1);
            if (c) {
                fprintf(stderr, "  [lz4-hc] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [lz4-hc] not available\n"); }
        }

        /* --- Zstd level 1 --- */
        if (args.compressor_mask & BENCH_COMP_ZSTD1) {
            bench_compressor_t *c = bench_zstd_create(1, 0);
            if (c) {
                fprintf(stderr, "  [zstd-1] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [zstd-1] not available\n"); }
        }

        /* --- Zstd level 3 --- */
        if (args.compressor_mask & BENCH_COMP_ZSTD3) {
            bench_compressor_t *c = bench_zstd_create(3, 0);
            if (c) {
                fprintf(stderr, "  [zstd-3] running...\n");
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [zstd-3] not available\n"); }
        }

        /* --- Zstd level 1 + dict --- */
        if (args.compressor_mask & BENCH_COMP_ZSTD1D) {
            bench_compressor_t *c = bench_zstd_create(1, 1);
            if (c) {
                fprintf(stderr, "  [zstd-1-dict] training + running...\n");
                if (c->train) c->train(c, wl, args.seed, args.train_count);
                run_compressor(c, wl, &args, reporter, results, &n_results);
                c->destroy(c);
            } else { fprintf(stderr, "  [zstd-1-dict] not available\n"); }
        }
    }

    bench_reporter_end(reporter);
    bench_reporter_close(reporter);
    if (out_fp != stdout) fclose(out_fp);

    /* CI gate check — includes COMP-* gates when comparison data is available */
    if (args.ci_check) {
        bench_ci_report_t ci;
        int ci_rc = bench_ci_check(results, (size_t)n_results, &ci);

        /* COMP-* gates: netc compress_mbs vs LZ4/zlib/Zstd on WL-001 */
        if (have_netc_wl001) {
            /* Find comparison results for WL-001 */
            for (int i = 0; i < n_results; i++) {
                if (results[i].workload != BENCH_WL_001) continue;
                if (strcmp(results[i].compressor, "netc") == 0) continue;

                /* netc must be faster than each competitor */
                const char *comp_name = results[i].compressor;
                char gate_id[32];
                char gate_desc[128];

                snprintf(gate_id,   sizeof(gate_id),
                         "COMP-%s", comp_name);
                snprintf(gate_desc, sizeof(gate_desc),
                         "netc compress_mbs (%.1f) > %s compress_mbs (%.1f)",
                         netc_wl001.compress_mbs, comp_name,
                         results[i].compress_mbs);

                if (ci.n_gates < BENCH_MAX_GATES) {
                    bench_gate_result_t *g = &ci.gates[ci.n_gates++];
                    g->gate_id     = comp_name;  /* use compressor name as gate label */
                    g->description = gate_desc;
                    g->actual      = netc_wl001.compress_mbs;
                    g->threshold   = results[i].compress_mbs;
                    g->passed      = (netc_wl001.compress_mbs > results[i].compress_mbs);
                    if (!g->passed) ci.all_passed = 0;
                }
            }
            ci_rc = ci.all_passed ? 0 : 1;
        }

        bench_ci_report_print(&ci);
        free(results);
        return ci_rc;
    }

    free(results);
    return 0;
}
