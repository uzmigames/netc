## 1. Phase 1B — Adaptive Frequency Normalization (quick win, lower risk)
- [x] 1.1 Analyze current freq_normalize() in netc_dict.c — understand Laplace smoothing baseline
- [x] 1.2 Implement two-phase floor+proportional normalization: all 256 symbols get freq=1 floor, remaining 3840 slots distributed proportionally to seen-only symbols
- [ ] ~~1.3 Add per-bucket `norm_threshold` to dict~~ (not needed — floor approach is bucket-agnostic)
- [x] 1.4 Benchmark ratio comparison: Laplace vs adaptive on all workloads (marginal gains on WL-004/WL-002, neutral elsewhere; PRNG variance makes A/B hard)
- [x] 1.5 Wire into compress/decompress paths (same freq_normalize used by all paths — no wiring needed)

## 2. Phase 1A — Order-2 Context (Trigram) tANS Encoding
- [ ] 2.1 Design trigram class mapping: `hash(prev0, prev1) % N_CLASSES` — evaluate N=4,8,16 classes
- [ ] 2.2 Extend dict training to accumulate order-2 frequency statistics per bucket
- [ ] 2.3 Train trigram class map using 2-byte context clustering (k-means on conditional distributions)
- [ ] 2.4 Dict format v6: serialize trigram class_map + tables (estimate memory: N_CLASSES × 16 buckets × 256 symbols × 2B = 128KB for N=16)
- [ ] 2.5 Add TRIGRAM packet types to compact header table
- [ ] 2.6 Encoder: try trigram tANS after bigram; pick smaller output
- [ ] 2.7 Decoder: decode trigram packets using 2-byte lookback context
- [ ] 2.8 Benchmark ratio comparison: bigram vs trigram on all workloads

## 3. Testing
- [x] 3.1 Unit tests: adaptive normalization round-trip (all 12 existing tests pass with new normalization)
- [ ] 3.2 Unit tests: trigram encode/decode correctness
- [ ] 3.3 Unit tests: dict v6 save/load round-trip, v5 backward compatibility
- [ ] 3.4 Cross-validation: train on subset, test on holdout — no ratio regression vs v5
- [ ] 3.5 Fuzz: trigram decode path with random bitstreams

## 4. Documentation
- [ ] 4.1 Update CHANGELOG
- [ ] 4.2 Update README (ratio table, features list)
- [ ] 4.3 Update RFC-002 (new ratio measurements)
