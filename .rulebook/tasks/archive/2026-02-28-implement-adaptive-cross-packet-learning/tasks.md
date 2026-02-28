## 1. Phase 1 — Adaptive tANS Frequency Tables

- [x] 1.1 Add `NETC_CFG_FLAG_ADAPTIVE` (bit 9, 0x200U) to netc.h
- [x] 1.2 Add frequency accumulators to `netc_ctx_t`: `uint32_t freq_accum[NETC_CTX_COUNT][256]` + `uint32_t freq_total[NETC_CTX_COUNT]`
- [x] 1.3 Add mutable `netc_tans_table_t adaptive_tables[NETC_CTX_COUNT]` to context (cloned from dict on creation)
- [x] 1.4 Implement `netc_ctx_freq_update(ctx, raw_bytes, size)` — counts bytes per bucket after each packet
- [x] 1.5 Implement `netc_ctx_tables_rebuild(ctx)` — blends accumulated freqs with dict baseline, normalizes, rebuilds tANS tables. Triggered every NETC_ADAPTIVE_INTERVAL=128 packets
- [x] 1.6 Wire encoder: when ADAPTIVE flag set, use `ctx->adaptive_tables` instead of `ctx->dict->tables`. Call `freq_update()` after each successful compress on the RAW bytes (not residuals)
- [x] 1.7 Wire decoder: same — use adaptive tables, call `freq_update()` after each successful decompress on the OUTPUT bytes
- [x] 1.8 Handle adaptive+bigram-PCTX: bigram tables stay static (from dict), only unigram tables adapt. Fixed bigram trial clobber bug using separate trial buffer.
- [x] 1.9 Benchmark Phase 1: adaptive vs static tested on all workloads
- [x] 1.10 Tune alpha blend ratio (3/4) and rebuild interval (128) based on benchmark results

## 2. Phase 2 — Adaptive LZP Hash Updates

- [x] 2.1 Add `netc_lzp_entry_t *adapt_lzp` to `netc_ctx_t` (NULL when not adaptive or no LZP in dict)
- [x] 2.2 Clone LZP table from dict into context on creation when ADAPTIVE flag set; boost confidence to 4
- [x] 2.3 Implement `netc_lzp_adaptive_update()` — confidence-based decay: hit→increment, miss→decrement, depleted→replace
- [x] 2.4 Wire encoder: use `netc_get_lzp_table(ctx)` for LZP XOR filter, call update after each packet
- [x] 2.5 Wire decoder: same — use adaptive LZP, call update after decode
- [x] 2.6 Test: adaptive LZP hit-rate >= dict baseline after 500 packets; enc/dec tables stay in sync

## 3. Phase 3 — Multi-Packet Delta (order-2 prediction)

- [x] 3.1 Add `prev2_pkt[NETC_MAX_PACKET_SIZE]` and `prev2_pkt_size` to `netc_ctx_t`
- [x] 3.2 Implement `netc_delta_encode_order2(prev2, prev, src, dst, size)` — linear extrapolation: `residual[i] = src[i] - (2*prev[i] - prev2[i])`
- [x] 3.3 Implement `netc_delta_decode_order2(prev2, prev, src, dst, size)` — inverse
- [x] 3.4 Wire into compressor: try order-2 when prev2 available and sizes match, compare with order-1, keep smaller
- [x] 3.5 Add packet flag or compact type for order-2 delta (distinguish from order-1 on wire)
- [x] 3.6 Wire into decompressor: decode order-2 when flagged
- [x] 3.7 Update prev2_pkt rotation: `prev2 = prev, prev = current` after each packet (both enc and dec)
- [x] 3.8 Benchmark Phase 3: measure delta residual improvement on game state workloads (WL-001 through WL-003 where fields evolve smoothly)

## 4. Integration & Testing

- [x] 4.1 Unit tests: adaptive tANS round-trip (verify encoder/decoder tables stay in sync over 100+ packets)
- [x] 4.2 Unit tests: adaptive LZP hit-rate improves over packet sequence
- [x] 4.3 Unit tests: order-2 delta round-trip (verify correct reconstruction)
- [x] 4.4 Unit tests: adaptive mode disabled (ADAPTIVE flag off) gives identical results to current behavior
- [x] 4.5 Unit tests: mixed adaptive + non-adaptive contexts sharing same dict
- [x] 4.6 Benchmark: sustained connection simulation (10K packets) — 16 tests, 10K-pkt adaptive round-trip verified
- [x] 4.7 Memory usage verification: ~1020 KB with all phases (within 1.5 MB hard limit; 512 KB target predated LZP+prev2)

## 5. Documentation

- [x] 5.1 Update CHANGELOG
- [x] 5.2 Update README (adaptive mode section, updated ratio table)
- [x] 5.3 Update RFC-002 (new ratio measurements with adaptive mode)
- [x] 5.4 Update API reference (NETC_CFG_FLAG_ADAPTIVE)
