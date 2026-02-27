/**
 * bench_corpus.h — Deterministic workload corpus generators.
 *
 * Implements WL-001 through WL-008 per RFC-002 §3.
 * All generators are seeded with a uint64_t seed so that the same seed
 * produces byte-for-byte identical packet sequences across runs.
 *
 * PRNG: splitmix64 (fast, good avalanche, no need for cryptographic quality)
 *
 * Usage:
 *   bench_corpus_t corpus;
 *   bench_corpus_init(&corpus, BENCH_WL_001, 42);
 *   while (bench_corpus_next(&corpus)) {
 *       // corpus.packet  — pointer to current packet bytes
 *       // corpus.pkt_len — number of bytes in packet
 *   }
 *   bench_corpus_destroy(&corpus);
 */

#ifndef BENCH_CORPUS_H
#define BENCH_CORPUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Workload identifiers (RFC-002 §3)
 * ========================================================================= */
typedef enum {
    BENCH_WL_001 = 1,   /* Game state 64 B  — entropy ~3.2 bits/byte  */
    BENCH_WL_002 = 2,   /* Game state 128 B — entropy ~3.8 bits/byte  */
    BENCH_WL_003 = 3,   /* Game state 256 B — entropy ~4.2 bits/byte  */
    BENCH_WL_004 = 4,   /* Financial tick 32 B — entropy ~2.8 bits/byte */
    BENCH_WL_005 = 5,   /* Telemetry 512 B  — entropy ~4.5 bits/byte  */
    BENCH_WL_006 = 6,   /* Random 128 B     — entropy ~8 bits/byte    */
    BENCH_WL_007 = 7,   /* Repetitive 128 B — entropy ~0.5 bits/byte  */
    BENCH_WL_008 = 8,   /* Mixed traffic 32–512 B, weighted           */
    BENCH_WL_ALL = 0,   /* Sentinel (run all workloads)               */
} bench_workload_t;

/* Maximum bytes any single packet can occupy across all workloads. */
#define BENCH_CORPUS_MAX_PKT  512u

/* Number of training packets (per RFC-002 §3). */
#define BENCH_CORPUS_TRAIN_N  50000u

/* =========================================================================
 * Corpus state
 * ========================================================================= */
typedef struct {
    bench_workload_t workload;
    uint64_t         rng;        /* splitmix64 state */
    uint64_t         seed;       /* original seed (stored for reset) */

    /* Current packet output */
    uint8_t          packet[BENCH_CORPUS_MAX_PKT];
    size_t           pkt_len;

    /* Internal state for WL-007 (cycle through patterns) */
    int              wl007_phase; /* 0=zeros, 1=ones, 2=run-length, 3=alt */

    /* Simulated moving-average price for WL-004 */
    double           wl004_price;
} bench_corpus_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/** Initialize corpus for the given workload and seed. */
void bench_corpus_init(bench_corpus_t *c, bench_workload_t wl, uint64_t seed);

/** Reset the corpus to the initial seed state (replay from beginning). */
void bench_corpus_reset(bench_corpus_t *c);

/** Generate the next packet.  Returns pkt_len (> 0) or 0 on error. */
size_t bench_corpus_next(bench_corpus_t *c);

/**
 * Generate a training corpus of 'n' packets into caller-supplied buffers.
 * Used to train compressor dictionaries before benchmarking.
 *
 *   bufs[i]   — pointer to i-th packet data  (points into 'storage')
 *   lens[i]   — length of i-th packet
 *   storage   — flat backing store, must be >= n * BENCH_CORPUS_MAX_PKT bytes
 */
void bench_corpus_train(bench_workload_t wl, uint64_t seed,
                        uint8_t **bufs, size_t *lens, size_t n,
                        uint8_t *storage);

/** Human-readable name of a workload (e.g. "WL-001 Game State 64B"). */
const char *bench_workload_name(bench_workload_t wl);

/** Fixed packet size for the given workload (0 for variable-length WL-008). */
size_t bench_workload_pkt_size(bench_workload_t wl);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_CORPUS_H */
