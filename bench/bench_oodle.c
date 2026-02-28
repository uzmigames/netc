/**
 * bench_oodle.c — OodleNetwork compressor adapter (task 3.7).
 *
 * All OodleNetwork API calls are guarded by NETC_BENCH_WITH_OODLE.
 * When the SDK is not present, both constructors return NULL and
 * bench_oodle_ci_gates() is a no-op.
 *
 * Training strategy (matches Oodle documentation):
 *   1. Concatenate all training packets into one contiguous buffer → "window".
 *   2. OodleNetwork1_Shared_SetWindow(shared, htbits, window, window_size)
 *   3. OodleNetwork1UDP_Train / OodleNetwork1TCP_Train with the SAME packets
 *      (but as separate pointer array — they must NOT overlap the window;
 *       here we pass pointers into a second copy of the training data).
 */

#include "bench_oodle.h"
#include "bench_corpus.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef NETC_BENCH_WITH_OODLE
#  include "oodle2net.h"
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

#define OODLE_MAX_PKT_SIZE  (BENCH_CORPUS_MAX_PKT)
/* Oodle encode overhead: OodleNetwork1_CompressedBufferSizeNeeded(src) */
#define OODLE_CMP_EXTRA     (256u)

/* =========================================================================
 * Adapter state type tag
 * ========================================================================= */
typedef enum { OODLE_UDP = 0, OODLE_TCP = 1 } oodle_mode_t;

/* =========================================================================
 * Adapter state
 * ========================================================================= */
typedef struct {
    bench_compressor_t      base;    /* MUST be first */
    oodle_mode_t            mode;
    int                     htbits;

#ifdef NETC_BENCH_WITH_OODLE
    /* Shared read-only dictionary (both UDP and TCP) */
    OodleNetwork1_Shared   *shared;
    void                   *window;      /* kept alive while shared is in use */
    OO_S32                  window_size;

    /* UDP: stateless per-packet — same state for encode and decode */
    OodleNetwork1UDP_State *udp_state;

    /* TCP: stateful per-channel */
    OodleNetwork1TCP_State *tcp_enc;
    OodleNetwork1TCP_State *tcp_dec;
    /* Snapshot of enc state right after training (for reset) */
    OodleNetwork1TCP_State *tcp_trained;
#endif
} oodle_state_t;

/* =========================================================================
 * Stub: no Oodle SDK
 * ========================================================================= */

#ifndef NETC_BENCH_WITH_OODLE

bench_compressor_t *bench_oodle_udp_create(int htbits)
{
    (void)htbits;
    return NULL;
}

bench_compressor_t *bench_oodle_tcp_create(int htbits)
{
    (void)htbits;
    return NULL;
}

void bench_oodle_ci_gates(const bench_result_t *netc_result,
                           const bench_result_t *oodle_result,
                           bench_ci_report_t    *report)
{
    (void)netc_result; (void)oodle_result; (void)report;
}

#else /* NETC_BENCH_WITH_OODLE */

/* =========================================================================
 * vtable: train
 *
 * Oodle requires:
 *   - window: a concatenation of representative packets (the "dictionary")
 *   - training packets: a *separate* copy of packet data (must not alias window)
 * ========================================================================= */

static int oodle_train(bench_compressor_t *base,
                       bench_workload_t    wl,
                       uint64_t            seed,
                       size_t              n)
{
    oodle_state_t *s = (oodle_state_t *)base;

    /* Build training corpus — two independent copies:
     *   window_buf  → used as dictionary via SetWindow (kept alive)
     *   train_buf   → used as training packets (can be freed after Train) */
    uint8_t  *window_buf  = (uint8_t *)malloc(n * BENCH_CORPUS_MAX_PKT);
    uint8_t  *train_buf   = (uint8_t *)malloc(n * BENCH_CORPUS_MAX_PKT);
    uint8_t **window_ptrs = (uint8_t **)malloc(n * sizeof(uint8_t *));
    uint8_t **train_ptrs  = (uint8_t **)malloc(n * sizeof(uint8_t *));
    OO_S32   *train_sizes = (OO_S32   *)malloc(n * sizeof(OO_S32));
    size_t   *lens        = (size_t   *)malloc(n * sizeof(size_t));

    if (!window_buf || !train_buf || !window_ptrs ||
        !train_ptrs || !train_sizes || !lens) {
        free(window_buf); free(train_buf);
        free(window_ptrs); free(train_ptrs);
        free(train_sizes); free(lens);
        return -1;
    }

    /* Fill window corpus */
    bench_corpus_train(wl, seed,      window_ptrs, lens, n, window_buf);
    /* Fill training corpus (different seed → different data → no aliasing) */
    bench_corpus_train(wl, seed ^ 1u, train_ptrs,  lens, n, train_buf);

    /* Concatenate window packets into one contiguous buffer */
    OO_S32  window_size = 0;
    uint8_t *window_concat = (uint8_t *)malloc(n * BENCH_CORPUS_MAX_PKT);
    if (!window_concat) {
        free(window_buf); free(train_buf);
        free(window_ptrs); free(train_ptrs);
        free(train_sizes); free(lens);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(window_concat + window_size, window_ptrs[i], lens[i]);
        window_size += (OO_S32)lens[i];
    }

    /* Convert sizes to OO_S32 */
    for (size_t i = 0; i < n; i++) {
        train_sizes[i] = (OO_S32)lens[i];
    }

    /* Allocate shared dictionary */
    OO_SINTa shared_bytes = OodleNetwork1_Shared_Size(s->htbits);
    s->shared      = (OodleNetwork1_Shared *)malloc((size_t)shared_bytes);
    s->window      = window_concat;
    s->window_size = window_size;

    if (!s->shared) {
        free(window_concat); free(window_buf); free(train_buf);
        free(window_ptrs); free(train_ptrs); free(train_sizes); free(lens);
        return -1;
    }

    OodleNetwork1_Shared_SetWindow(s->shared, s->htbits,
                                   window_concat, window_size);

    if (s->mode == OODLE_UDP) {
        OO_SINTa state_bytes  = OodleNetwork1UDP_State_Size();
        s->udp_state = (OodleNetwork1UDP_State *)malloc((size_t)state_bytes);
        if (!s->udp_state) {
            free(window_concat); free(window_buf); free(train_buf);
            free(window_ptrs); free(train_ptrs); free(train_sizes); free(lens);
            return -1;
        }
        OodleNetwork1UDP_Train(s->udp_state, s->shared,
                               (const void **)train_ptrs, train_sizes,
                               (OO_S32)n);
    } else {
        OO_SINTa state_bytes = OodleNetwork1TCP_State_Size();
        s->tcp_enc     = (OodleNetwork1TCP_State *)malloc((size_t)state_bytes);
        s->tcp_dec     = (OodleNetwork1TCP_State *)malloc((size_t)state_bytes);
        s->tcp_trained = (OodleNetwork1TCP_State *)malloc((size_t)state_bytes);
        if (!s->tcp_enc || !s->tcp_dec || !s->tcp_trained) {
            free(window_concat); free(window_buf); free(train_buf);
            free(window_ptrs); free(train_ptrs); free(train_sizes); free(lens);
            return -1;
        }
        OodleNetwork1TCP_Train(s->tcp_enc, s->shared,
                               (const void **)train_ptrs, train_sizes,
                               (OO_S32)n);
        /* Save snapshot for reset */
        memcpy(s->tcp_trained, s->tcp_enc, (size_t)state_bytes);
        memcpy(s->tcp_dec,     s->tcp_enc, (size_t)state_bytes);
    }

    free(window_buf); free(train_buf);
    free(window_ptrs); free(train_ptrs);
    free(train_sizes); free(lens);
    return 0;
}

/* =========================================================================
 * vtable: reset
 * ========================================================================= */

static void oodle_reset(bench_compressor_t *base)
{
    oodle_state_t *s = (oodle_state_t *)base;
    if (s->mode == OODLE_TCP && s->tcp_trained && s->tcp_enc && s->tcp_dec) {
        OO_SINTa state_bytes = OodleNetwork1TCP_State_Size();
        /* Restore enc and dec to post-training snapshot */
        memcpy(s->tcp_enc, s->tcp_trained, (size_t)state_bytes);
        memcpy(s->tcp_dec, s->tcp_trained, (size_t)state_bytes);
    }
    /* UDP is stateless — nothing to reset */
}

/* =========================================================================
 * vtable: compress / decompress
 * ========================================================================= */

/* Oodle is headerless — rawLen must be transmitted out-of-band.
 * We prepend a 4-byte little-endian rawLen to the compressed payload so
 * decompress can reconstruct it without external state. */
#define OODLE_HDR_SIZE  4u

static size_t oodle_compress(bench_compressor_t *base,
                              const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap)
{
    oodle_state_t *s = (oodle_state_t *)base;
    if (dst_cap < OODLE_HDR_SIZE) return 0;

    /* Write rawLen header */
    uint32_t raw32 = (uint32_t)src_len;
    memcpy(dst, &raw32, 4);

    OO_SINTa out_len;
    if (s->mode == OODLE_UDP) {
        out_len = OodleNetwork1UDP_Encode(
            s->udp_state, s->shared,
            src, (OO_SINTa)src_len,
            dst + OODLE_HDR_SIZE);
    } else {
        out_len = OodleNetwork1TCP_Encode(
            s->tcp_enc, s->shared,
            src, (OO_SINTa)src_len,
            dst + OODLE_HDR_SIZE);
    }
    return (out_len > 0) ? (size_t)out_len + OODLE_HDR_SIZE : 0;
}

static size_t oodle_decompress(bench_compressor_t *base,
                                const uint8_t *src, size_t src_len,
                                uint8_t *dst, size_t dst_cap)
{
    oodle_state_t *s = (oodle_state_t *)base;
    if (src_len <= OODLE_HDR_SIZE) return 0;

    /* Read rawLen from header */
    uint32_t raw32 = 0;
    memcpy(&raw32, src, 4);
    OO_SINTa raw_len = (OO_SINTa)raw32;
    if ((size_t)raw_len > dst_cap) return 0;

    OO_BOOL ok;
    if (s->mode == OODLE_UDP) {
        ok = OodleNetwork1UDP_Decode(
            s->udp_state, s->shared,
            src + OODLE_HDR_SIZE, (OO_SINTa)(src_len - OODLE_HDR_SIZE),
            dst, raw_len);
    } else {
        ok = OodleNetwork1TCP_Decode(
            s->tcp_dec, s->shared,
            src + OODLE_HDR_SIZE, (OO_SINTa)(src_len - OODLE_HDR_SIZE),
            dst, raw_len);
    }
    return ok ? (size_t)raw_len : 0;
}

/* =========================================================================
 * vtable: destroy
 * ========================================================================= */

static void oodle_destroy(bench_compressor_t *base)
{
    oodle_state_t *s = (oodle_state_t *)base;
    free(s->window);
    free(s->shared);
    free(s->udp_state);
    free(s->tcp_enc);
    free(s->tcp_dec);
    free(s->tcp_trained);
    free(s);
}

/* =========================================================================
 * Constructors
 * ========================================================================= */

static bench_compressor_t *oodle_create(oodle_mode_t mode, int htbits)
{
    oodle_state_t *s = (oodle_state_t *)calloc(1, sizeof(oodle_state_t));
    if (!s) return NULL;

    s->mode   = mode;
    s->htbits = htbits;

    s->base.state      = s;
    s->base.train      = oodle_train;
    s->base.reset      = oodle_reset;
    s->base.compress   = oodle_compress;
    s->base.decompress = oodle_decompress;
    s->base.destroy    = oodle_destroy;

    if (mode == OODLE_UDP) {
        s->base.name = "oodle-udp";
        s->base.cfg  = "OodleNetwork1UDP htbits=17";
    } else {
        s->base.name = "oodle-tcp";
        s->base.cfg  = "OodleNetwork1TCP htbits=17";
    }

    return &s->base;
}

bench_compressor_t *bench_oodle_udp_create(int htbits)
{
    return oodle_create(OODLE_UDP, htbits);
}

bench_compressor_t *bench_oodle_tcp_create(int htbits)
{
    return oodle_create(OODLE_TCP, htbits);
}

/* =========================================================================
 * OODLE-* CI gates
 * ========================================================================= */

void bench_oodle_ci_gates(const bench_result_t *netc_result,
                           const bench_result_t *oodle_result,
                           bench_ci_report_t    *report)
{
    if (!netc_result || !oodle_result || !report) return;
    if (report->n_gates + 3 > BENCH_MAX_GATES) return;

    {
        /* OODLE-01: netc ratio <= oodle ratio (netc compresses at least as well) */
        bench_gate_result_t *g = &report->gates[report->n_gates++];
        g->gate_id     = "OODLE-01";
        g->description = "netc ratio <= oodle ratio (WL-001)";
        g->actual      = netc_result->ratio;
        g->threshold   = oodle_result->ratio;
        g->passed      = (netc_result->ratio <= oodle_result->ratio) ? 1 : 0;
        if (!g->passed) report->all_passed = 0;
    }
    {
        /* OODLE-02: netc compress MB/s >= oodle compress MB/s */
        bench_gate_result_t *g = &report->gates[report->n_gates++];
        g->gate_id     = "OODLE-02";
        g->description = "netc compress MB/s >= oodle compress MB/s (WL-001)";
        g->actual      = netc_result->compress_mbs;
        g->threshold   = oodle_result->compress_mbs;
        g->passed      = (netc_result->compress_mbs >= oodle_result->compress_mbs) ? 1 : 0;
        if (!g->passed) report->all_passed = 0;
    }
    {
        /* OODLE-03: netc decompress MB/s >= oodle decompress MB/s */
        bench_gate_result_t *g = &report->gates[report->n_gates++];
        g->gate_id     = "OODLE-03";
        g->description = "netc decompress MB/s >= oodle decompress MB/s (WL-001)";
        g->actual      = netc_result->decompress_mbs;
        g->threshold   = oodle_result->decompress_mbs;
        g->passed      = (netc_result->decompress_mbs >= oodle_result->decompress_mbs) ? 1 : 0;
        if (!g->passed) report->all_passed = 0;
    }
}

#endif /* NETC_BENCH_WITH_OODLE */
