/**
 * bench_huffman.h — Reference static Huffman compressor adapter.
 *
 * Implements a canonical byte-level Huffman codec trained on the same corpus
 * as netc. Uses a 256-symbol table, length-limited to 15 bits.
 * The codec is always available (no external dependency).
 *
 * This serves as a performance floor: netc should substantially outperform
 * static Huffman on structured game packets due to the tANS fractional-bit
 * advantage and delta prediction.
 *
 * Adapter name: "huffman-static"
 */

#ifndef BENCH_HUFFMAN_H
#define BENCH_HUFFMAN_H

#include "bench_compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a static Huffman adapter.
 * Call c->train(c, wl, seed, n) before the benchmark loop to build the table.
 * Without training the adapter compresses with a flat (uniform) code table.
 * Caller must call c->destroy(c) when done.
 */
bench_compressor_t *bench_huffman_create(void);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_HUFFMAN_H */
