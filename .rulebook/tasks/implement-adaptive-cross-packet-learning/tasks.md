## 1. Phase 1 — Adaptive tANS Frequency Tables

- [ ] 1.1 Add `NETC_CFG_FLAG_ADAPTIVE` (bit 9, 0x200U) to netc.h
- [ ] 1.2 Add frequency accumulators to `netc_ctx_t`: `uint32_t freq_accum[NETC_CTX_COUNT][256]` + `uint32_t freq_total[NETC_CTX_COUNT]`
- [ ] 1.3 Add mutable `netc_tans_table_t adaptive_tables[NETC_CTX_COUNT]` to context (cloned from dict on creation)
- [ ] 1.4 Implement `netc_ctx_freq_update(ctx, raw_bytes, size)` — counts bytes per bucket after each packet
- [ ] 1.5 Implement `netc_ctx_tables_rebuild(ctx)` — blends accumulated freqs with dict baseline, normalizes, rebuilds tANS tables. Triggered every NETC_ADAPTIVE_INTERVAL packets (start with 64)
- [ ] 1.6 Wire encoder: when ADAPTIVE flag set, use `ctx->adaptive_tables` instead of `ctx->dict->tables`. Call `freq_update()` after each successful compress on the RAW bytes (not residuals)
- [ ] 1.7 Wire decoder: same — use adaptive tables, call `freq_update()` after each successful decompress on the OUTPUT bytes
- [ ] 1.8 Handle adaptive+bigram-PCTX: adaptive bigram tables need `adaptive_bigram_tables[NETC_CTX_COUNT][NETC_BIGRAM_CTX_COUNT]` — evaluate if cost is justified or bigram stays static
- [ ] 1.9 Benchmark Phase 1: compare adaptive vs static on all workloads at count=1000+ (simulate sustained connection)
- [ ] 1.10 Tune alpha blend ratio and rebuild interval based on benchmark results

## 2. Phase 2 — Adaptive LZP Hash Updates

- [ ] 2.1 Add `netc_lzp_entry_t *adaptive_lzp` to `netc_ctx_t` (NULL when not adaptive)
- [ ] 2.2 Clone LZP table from dict into context on first packet when ADAPTIVE flag set
- [ ] 2.3 Implement `netc_lzp_update(lzp_table, raw_bytes, size)` — on prediction miss, overwrite entry with actual byte
- [ ] 2.4 Wire encoder: use `ctx->adaptive_lzp` for LZP XOR filter when available
- [ ] 2.5 Wire decoder: same — use adaptive LZP, call update after decode
- [ ] 2.6 Benchmark Phase 2: measure LZP hit rate improvement over time (packets 1-100, 100-1000, 1000+)

## 3. Phase 3 — Multi-Packet Delta (order-2 prediction)

- [ ] 3.1 Add `prev2_pkt[NETC_MAX_PACKET_SIZE]` and `prev2_pkt_size` to `netc_ctx_t`
- [ ] 3.2 Implement `netc_delta_encode_order2(prev2, prev, src, dst, size)` — linear extrapolation: `residual[i] = src[i] - (2*prev[i] - prev2[i])`
- [ ] 3.3 Implement `netc_delta_decode_order2(prev2, prev, src, dst, size)` — inverse
- [ ] 3.4 Wire into compressor: try order-2 when prev2 available and sizes match, compare with order-1, keep smaller
- [ ] 3.5 Add packet flag or compact type for order-2 delta (distinguish from order-1 on wire)
- [ ] 3.6 Wire into decompressor: decode order-2 when flagged
- [ ] 3.7 Update prev2_pkt rotation: `prev2 = prev, prev = current` after each packet (both enc and dec)
- [ ] 3.8 Benchmark Phase 3: measure delta residual improvement on game state workloads (WL-001 through WL-003 where fields evolve smoothly)

## 4. Integration & Testing

- [ ] 4.1 Unit tests: adaptive tANS round-trip (verify encoder/decoder tables stay in sync over 100+ packets)
- [ ] 4.2 Unit tests: adaptive LZP hit-rate improves over packet sequence
- [ ] 4.3 Unit tests: order-2 delta round-trip (verify correct reconstruction)
- [ ] 4.4 Unit tests: adaptive mode disabled (ADAPTIVE flag off) gives identical results to current behavior
- [ ] 4.5 Unit tests: mixed adaptive + non-adaptive contexts sharing same dict
- [ ] 4.6 Benchmark: sustained connection simulation (10K packets) — netc adaptive vs Oodle TCP on all workloads
- [ ] 4.7 Memory usage verification: total context memory ≤ 512 KB with all phases enabled

## 5. Documentation

- [ ] 5.1 Update CHANGELOG
- [ ] 5.2 Update README (adaptive mode section, updated ratio table)
- [ ] 5.3 Update RFC-002 (new ratio measurements with adaptive mode)
- [ ] 5.4 Update API reference (NETC_CFG_FLAG_ADAPTIVE)
