## 1. Phase 1B — Adaptive Frequency Normalization (quick win, lower risk)
- [x] 1.1 Analyze current freq_normalize() in netc_dict.c — understand Laplace smoothing baseline
- [x] 1.2 Implement two-phase floor+proportional normalization: all 256 symbols get freq=1 floor, remaining 3840 slots distributed proportionally to seen-only symbols
- [ ] ~~1.3 Add per-bucket `norm_threshold` to dict~~ (not needed — floor approach is bucket-agnostic)
- [x] 1.4 Benchmark ratio comparison: Laplace vs adaptive on all workloads (marginal gains on WL-004/WL-002, neutral elsewhere; PRNG variance makes A/B hard)
- [x] 1.5 Wire into compress/decompress paths (same freq_normalize used by all paths — no wiring needed)

## 2. Phase 1A — Bigram-PCTX Per-Position Context-Adaptive Encoding
*Revised: Instead of order-2 trigram with new table types, implemented bigram-PCTX —
per-position table switching using BOTH bucket AND bigram class. Uses existing v5 bigram
tables, no dict format change needed.*

- [x] 2.1 Design bigram-PCTX: per-byte table = bigram_tables[bucket][bigram_class(prev_byte)]
- [x] 2.2 Implement netc_tans_encode_pctx_bigram() in netc_tans.c
- [x] 2.3 Implement netc_tans_decode_pctx_bigram() in netc_tans.c
- [x] 2.4 Add PCTX+BIGRAM compact packet types (0xD0-0xD3: PCTX+BG, PCTX+BG+DELTA, PCTX+LZP+BG, PCTX+LZP+BG+DELTA)
- [x] 2.5 Wire into compressor: try bigram-PCTX alongside unigram PCTX, keep smaller
- [x] 2.6 Wire into decompressor: check BIGRAM flag in PCTX decode path
- [x] 2.7 Handle used_mreg==3 (PCTX+BIGRAM) in all header emission paths
- [x] 2.8 Benchmark: 1-7% ratio improvement across all workloads; beats Oodle UDP on WL-004 and WL-005

## 3. Testing
- [x] 3.1 Unit tests: adaptive normalization round-trip (all 12 existing tests pass with new normalization)
- [x] 3.2 Unit tests: bigram-PCTX compact round-trip (5 packet sizes: 32-512B)
- [x] 3.3 Unit tests: bigram-PCTX + delta round-trip
- [x] 3.4 Unit tests: bigram-PCTX multi-packet sequence (20 packets)
- [x] 3.5 Unit tests: bigram-PCTX legacy header round-trip

## 4. Documentation
- [x] 4.1 Update CHANGELOG
- [x] 4.2 Update README (ratio table, gap table, features list)
- [x] 4.3 Update RFC-002 §10.4 with ratio measurements and technique descriptions
