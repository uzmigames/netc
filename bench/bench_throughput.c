/**
 * bench_throughput.c — Sustained throughput and Mpps benchmarks (RFC-002 §5 tasks 4.2–4.3).
 */

#include "bench_throughput.h"
#include "bench_runner.h"
#include "bench_corpus.h"
#include "bench_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scratch buffer size — must accommodate any workload packet */
#define SCRATCH_CAP (BENCH_CORPUS_MAX_PKT * 2u + 256u)

/* =========================================================================
 * Internal: bulk run (compress then decompress passes)
 * ========================================================================= */

static int throughput_run_internal(const bench_throughput_cfg_t *cfg,
                                   bench_workload_t               wl,
                                   bench_compressor_t            *c,
                                   bench_throughput_result_t     *out)
{
    /* Use eval seed so test packets differ from training corpus */
    uint64_t eval_seed = cfg->seed + BENCH_EVAL_SEED_OFFSET;

    bench_corpus_t corpus;
    bench_corpus_init(&corpus, wl, eval_seed);

    uint8_t cmp_buf[SCRATCH_CAP];
    uint8_t dec_buf[BENCH_CORPUS_MAX_PKT];

    /* ---- Warm-up pass (compress only, not counted) ---- */
    for (size_t i = 0; i < cfg->warmup; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) break;
        c->compress(c, corpus.packet, plen, cmp_buf, sizeof(cmp_buf));
    }

    /* Reset corpus to measurement position */
    bench_corpus_reset(&corpus);

    /* ---- Compression pass ---- */
    uint64_t total_orig  = 0;
    uint64_t total_cmp   = 0;
    size_t   packets     = 0;

    /* Pre-generate packets into a buffer so the decompress pass can reuse them */
    size_t   max_n   = cfg->count;
    uint8_t *pkt_buf = (uint8_t *)malloc(max_n * BENCH_CORPUS_MAX_PKT);
    uint8_t *cmp_out = (uint8_t *)malloc(max_n * SCRATCH_CAP);
    size_t  *pkt_len = (size_t  *)malloc(max_n * sizeof(size_t));
    size_t  *cmp_len = (size_t  *)malloc(max_n * sizeof(size_t));

    if (!pkt_buf || !cmp_out || !pkt_len || !cmp_len) {
        free(pkt_buf); free(cmp_out); free(pkt_len); free(cmp_len);
        return -1;
    }

    /* Generate packets */
    for (size_t i = 0; i < max_n; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) plen = bench_corpus_next(&corpus);  /* retry once */
        pkt_len[i] = plen;
        memcpy(pkt_buf + i * BENCH_CORPUS_MAX_PKT, corpus.packet, plen);
    }

    /* Time compression */
    uint64_t t0 = bench_now_ns();
    for (size_t i = 0; i < max_n; i++) {
        const uint8_t *pkt = pkt_buf + i * BENCH_CORPUS_MAX_PKT;
        size_t plen = pkt_len[i];
        uint8_t *co  = cmp_out + i * SCRATCH_CAP;
        size_t clen = c->compress(c, pkt, plen, co, SCRATCH_CAP);
        if (clen == 0) {
            /* Store raw as fallback */
            memcpy(co, pkt, plen);
            clen = plen;
        }
        cmp_len[i] = clen;
        total_orig += plen;
        total_cmp  += clen;
        packets++;
    }
    uint64_t t1 = bench_now_ns();
    double compress_elapsed = (double)(t1 - t0) * 1e-9;

    /* Time decompression */
    uint64_t t2 = bench_now_ns();
    for (size_t i = 0; i < max_n; i++) {
        uint8_t *co   = cmp_out + i * SCRATCH_CAP;
        size_t clen   = cmp_len[i];
        size_t dlen   = c->decompress(c, co, clen, dec_buf, sizeof(dec_buf));
        (void)dlen;
    }
    uint64_t t3 = bench_now_ns();
    double decompress_elapsed = (double)(t3 - t2) * 1e-9;

    free(pkt_buf); free(cmp_out); free(pkt_len); free(cmp_len);

    /* Fill output */
    out->compressor    = c->name;
    out->compressor_cfg = c->cfg;
    out->workload      = wl;
    out->packets       = packets;
    out->original_bytes   = total_orig;
    out->compressed_bytes = total_cmp;
    out->ratio         = total_orig > 0 ? (double)total_cmp / (double)total_orig : 1.0;

    out->compress_elapsed_s   = compress_elapsed;
    out->decompress_elapsed_s = decompress_elapsed;

    double orig_mb = (double)total_orig / (1024.0 * 1024.0);
    double pkt_m   = (double)packets / 1e6;

    out->compress_mbs   = compress_elapsed   > 0 ? orig_mb / compress_elapsed   : 0.0;
    out->decompress_mbs = decompress_elapsed > 0 ? orig_mb / decompress_elapsed : 0.0;
    out->compress_mpps   = compress_elapsed   > 0 ? pkt_m / compress_elapsed   : 0.0;
    out->decompress_mpps = decompress_elapsed > 0 ? pkt_m / decompress_elapsed : 0.0;

    return 0;
}

/* =========================================================================
 * Public: bench_throughput_run
 * ========================================================================= */

int bench_throughput_run(const bench_throughput_cfg_t *cfg,
                         bench_workload_t               wl,
                         bench_compressor_t            *c,
                         bench_throughput_result_t     *out)
{
    bench_timer_init();
    return throughput_run_internal(cfg, wl, c, out);
}

/* =========================================================================
 * Public: bench_mpps_run
 * ========================================================================= */

int bench_mpps_run(bench_workload_t           wl,
                   bench_compressor_t         *c,
                   uint64_t                    seed,
                   bench_throughput_result_t  *out)
{
    bench_throughput_cfg_t cfg;
    cfg.warmup = 10000u;
    cfg.count  = 1000000u;  /* RFC-002 §5.3: exactly 1 M packets */
    cfg.seed   = seed;

    bench_timer_init();
    return throughput_run_internal(&cfg, wl, c, out);
}

/* =========================================================================
 * Public: bench_throughput_print
 * ========================================================================= */

void bench_throughput_print(const bench_throughput_result_t *r)
{
    printf("%-20s %-12s  packets=%7llu  orig=%7.2f MB  ratio=%.3f\n"
           "  compress:   %7.1f MB/s  %6.3f Mpps  (%.3f s)\n"
           "  decompress: %7.1f MB/s  %6.3f Mpps  (%.3f s)\n",
           r->compressor, r->compressor_cfg,
           (unsigned long long)r->packets,
           (double)r->original_bytes / (1024.0 * 1024.0),
           r->ratio,
           r->compress_mbs,   r->compress_mpps,   r->compress_elapsed_s,
           r->decompress_mbs, r->decompress_mpps, r->decompress_elapsed_s);
}
