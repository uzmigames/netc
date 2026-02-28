/**
 * bench_compressor.c — Generic compressor timing loop (bench_run_generic).
 *
 * Mirrors bench_runner.c but uses the bench_compressor_t vtable instead of
 * the netc-specific bench_netc_t, so any compressor can be benchmarked.
 */

#include "bench_compressor.h"
#include "bench_timer.h"
#include "bench_stats.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int bench_run_generic(const bench_generic_cfg_t *cfg,
                      bench_workload_t            wl,
                      bench_compressor_t         *c,
                      bench_result_t             *out)
{
    if (!cfg || !c || !out) return -1;

    size_t warmup = cfg->warmup;
    size_t count  = cfg->count;
    if (count == 0) count = 100000u;

    uint64_t *c_samples  = (uint64_t *)malloc(count * sizeof(uint64_t));
    uint64_t *d_samples  = (uint64_t *)malloc(count * sizeof(uint64_t));
    uint8_t  *orig_buf   = (uint8_t  *)malloc(BENCH_CORPUS_MAX_PKT);
    uint8_t  *comp_buf   = (uint8_t  *)malloc(BENCH_CORPUS_MAX_PKT + 64u);
    uint8_t  *decomp_buf = (uint8_t  *)malloc(BENCH_CORPUS_MAX_PKT);

    if (!c_samples || !d_samples || !orig_buf || !comp_buf || !decomp_buf) {
        free(c_samples); free(d_samples);
        free(orig_buf); free(comp_buf); free(decomp_buf);
        return -1;
    }

    bench_corpus_t corpus;
    bench_corpus_init(&corpus, wl, cfg->seed);
    if (c->reset) c->reset(c);

    uint64_t total_orig  = 0;
    uint64_t total_comp  = 0;
    int      safety_ok   = 1;

    bench_timer_init();

    /* Warmup (not timed) */
    for (size_t i = 0; i < warmup; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) { bench_corpus_reset(&corpus); plen = bench_corpus_next(&corpus); }
        memcpy(orig_buf, corpus.packet, plen);
        size_t clen = c->compress(c, orig_buf, plen,
                                  comp_buf, BENCH_CORPUS_MAX_PKT + 64u);
        if (clen > 0) {
            c->decompress(c, comp_buf, clen, decomp_buf, BENCH_CORPUS_MAX_PKT);
        }
    }

    bench_corpus_reset(&corpus);
    if (c->reset) c->reset(c);

    /* Measurement */
    for (size_t i = 0; i < count; i++) {
        size_t plen = bench_corpus_next(&corpus);
        if (plen == 0) { bench_corpus_reset(&corpus); plen = bench_corpus_next(&corpus); }
        memcpy(orig_buf, corpus.packet, plen);

        uint64_t t0 = bench_now_ns();
        size_t clen = c->compress(c, orig_buf, plen,
                                  comp_buf, BENCH_CORPUS_MAX_PKT + 64u);
        uint64_t t1 = bench_now_ns();
        c_samples[i] = (t1 >= t0) ? (t1 - t0) : 0;

        if (clen == 0) {
            /* Compressor signalled incompressible — store raw as passthrough.
             * Count the compress time but skip the round-trip check for this
             * packet (incompressibility is not a safety violation). */
            memcpy(comp_buf, orig_buf, plen);
            clen = plen;
            d_samples[i] = 0;
            total_orig += plen;
            total_comp += plen;
            continue;
        }

        total_orig += plen;
        total_comp += clen;

        uint64_t t2 = bench_now_ns();
        size_t dlen = c->decompress(c, comp_buf, clen,
                                    decomp_buf, BENCH_CORPUS_MAX_PKT);
        uint64_t t3 = bench_now_ns();
        d_samples[i] = (t3 >= t2) ? (t3 - t2) : 0;

        if (dlen != plen || memcmp(orig_buf, decomp_buf, plen) != 0) {
            safety_ok = 0;
        }
    }

    bench_stats_compute(&out->compress,   c_samples, count);
    bench_stats_compute(&out->decompress, d_samples, count);

    out->compressor      = c->name;
    out->compressor_cfg  = c->cfg ? c->cfg : "";
    out->workload        = wl;
    out->pkt_size        = bench_workload_pkt_size(wl);
    out->count           = (uint64_t)count;
    out->seed            = cfg->seed;
    out->original_bytes  = total_orig;
    out->compressed_bytes = total_comp;
    out->ratio           = bench_stats_ratio(total_orig, total_comp);

    size_t bpp = out->pkt_size;
    if (bpp == 0 && count > 0) bpp = (size_t)(total_orig / count);
    out->compress_mbs    = bench_stats_throughput_mbs(bpp, out->compress.mean_ns);
    out->compress_mpps   = bench_stats_mpps(out->compress.mean_ns);
    out->decompress_mbs  = bench_stats_throughput_mbs(bpp, out->decompress.mean_ns);
    out->decompress_mpps = bench_stats_mpps(out->decompress.mean_ns);

    if (!safety_ok) {
        fprintf(stderr, "[bench] SAFETY-01 FAIL: round-trip mismatch (%s, %s)\n",
                c->name, bench_workload_name(wl));
    }

    free(c_samples); free(d_samples);
    free(orig_buf); free(comp_buf); free(decomp_buf);
    return safety_ok ? 0 : -1;
}
