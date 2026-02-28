/**
 * bench_oodle.h — OodleNetwork compressor adapter.
 *
 * Compiled only when NETC_BENCH_WITH_OODLE is ON in CMake.
 * Requires UE5_OODLE_SDK path or NETC_OODLE_SDK_PATH to point to a directory
 * containing the Oodle SDK headers and library.
 *
 * Two sub-adapters are provided:
 *
 *   bench_oodle_udp_create(htbits)
 *     — OodleNetwork1UDP_Train / OodleNetwork1UDP_Encode / OodleNetwork1UDP_Decode
 *       Stateless per-packet encoding, state captured in a shared state object.
 *
 *   bench_oodle_tcp_create(htbits)
 *     — OodleNetwork1TCP_Train / OodleNetwork1TCP_Encode / OodleNetwork1TCP_Decode
 *       Stateful stream encoding (state carried across packets).
 *
 * When NETC_BENCH_WITH_OODLE is not defined, both constructors return NULL.
 *
 * OODLE-* CI gates (RFC-002 §6.3):
 *   OODLE-01: netc ratio ≤ oodle ratio          (netc compresses at least as well)
 *   OODLE-02: netc compress_mbs ≥ oodle mbs    (netc is at least as fast to compress)
 *   OODLE-03: netc decompress_mbs ≥ oodle mbs  (netc is at least as fast to decompress)
 */

#ifndef BENCH_OODLE_H
#define BENCH_OODLE_H

#include "bench_compressor.h"
#include "bench_reporter.h"
#include "bench_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Adapter constructors
 * ========================================================================= */

/**
 * Create an OodleNetwork1UDP adapter.
 * @param htbits  Hash table size in bits (16-23); 17-18 typical for small packets.
 * Returns NULL if NETC_BENCH_WITH_OODLE is not defined.
 */
bench_compressor_t *bench_oodle_udp_create(int htbits);

/**
 * Create an OodleNetwork1TCP adapter.
 * @param htbits  Hash table size in bits.
 * Returns NULL if NETC_BENCH_WITH_OODLE is not defined.
 */
bench_compressor_t *bench_oodle_tcp_create(int htbits);

/* =========================================================================
 * OODLE-* CI gates
 * ========================================================================= */

/**
 * Run OODLE-01 / OODLE-02 / OODLE-03 gates.
 *
 * Compares netc_result against oodle_result on WL-001.
 * Adds gate results to an existing bench_ci_report_t (appends to report->gates[]).
 *
 * @param netc_result   WL-001 result for netc
 * @param oodle_result  WL-001 result for oodle (UDP or TCP)
 * @param report        CI report to append gates into
 */
void bench_oodle_ci_gates(const bench_result_t *netc_result,
                           const bench_result_t *oodle_result,
                           bench_ci_report_t    *report);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_OODLE_H */
