## 1. Adaptive tANS Table Size Selection
- [ ] 1.1 Add 10-bit (1024-entry) tANS table support in `netc_tans.h`
  - Parameterize `NETC_TANS_TABLE_LOG` (currently hardcoded to 12)
  - Add `netc_tans_build_table_ex()` accepting table_log parameter
  - Frequency normalization: rescale 4096-sum tables to 1024-sum
- [ ] 1.2 Implement table size competition in `try_tans_single_with_table()`
  - For packets ≤ 128B: try both 12-bit and 10-bit tables
  - Keep whichever produces smaller compressed output
  - Encode selected table size in compact packet type (1 bit or new type range)
- [ ] 1.3 Update compact packet type table for 10-bit variants
  - Either reuse existing types with a flag bit, or allocate 0xB0-0xBF range
- [ ] 1.4 Update decompressor to handle 10-bit table decode
  - Build decode table from dict frequencies rescaled to 1024
  - State range becomes [1024, 2048) instead of [4096, 8192)
- [ ] 1.5 Benchmark: measure ratio delta on WL-001, WL-002, WL-004

## 2. Refined Bigram Quantization (8 classes)
- [ ] 2.1 Add trained bigram class mapping to dictionary
  - During `netc_dict_train()`: cluster previous bytes by conditional distribution similarity
  - Store 256-entry `bigram_class_map[256]` in dict (256 bytes, maps prev_byte → class 0-7)
  - Simple clustering: sort prev_bytes by frequency, assign to 8 equal-sized groups
- [ ] 2.2 Update bigram table training to use 8 classes instead of 4
  - Expand bigram tables: 16 buckets × 8 classes × 256 × uint16 (double current size)
  - Update `netc_bigram_class()` to use dict->bigram_class_map lookup
- [ ] 2.3 Update dict serialization (v5) for bigram class map + 8-class tables
- [ ] 2.4 Backward compat: v4 dicts continue to use 4-class scheme
- [ ] 2.5 Benchmark: measure ratio delta on WL-001, WL-002

## 3. Fine-Grained Bucketing for Small Packets
- [ ] 3.1 Define 32-bucket scheme for packets ≤ 128B
  - Bucket boundaries: every 4 bytes (0-3, 4-7, ..., 124-127)
  - Store as secondary frequency tables in dict
- [ ] 3.2 Train 32-bucket tables alongside existing 16-bucket tables
  - Only train on packets ≤ 128B from training corpus
  - Fallback: if < 1000 small training packets, skip 32-bucket training
- [ ] 3.3 Add bucket scheme selection in compressor
  - For packets ≤ 128B with 32-bucket dict: try 32-bucket, keep if smaller
  - Encode bucket scheme in compact header (1 bit or new type range)
- [ ] 3.4 Update decompressor for 32-bucket decode
- [ ] 3.5 Benchmark: measure ratio delta on WL-001, WL-002, WL-004

## 4. Dual-Dictionary Strategy (optional, lower priority)
- [ ] 4.1 Evaluate: does combining Phase 1+2+3 close enough of the gap?
  - If cumulative gain > 3%, this phase may not be needed
- [ ] 4.2 If needed: train separate small-packet dict (≤ 128B)
  - Optimized bucket boundaries, 10-bit tables, 8-class bigram
- [ ] 4.3 Dict selection bit in compact header
- [ ] 4.4 Benchmark improvement vs single-dict approach

## 5. Testing
- [ ] 5.1 Test 10-bit tANS table build + encode + decode round-trip
- [ ] 5.2 Test 10-bit vs 12-bit competition produces correct output
- [ ] 5.3 Test 8-class bigram encode/decode round-trip
- [ ] 5.4 Test 32-bucket scheme encode/decode round-trip
- [ ] 5.5 Test backward compat: v4 dicts work with new code
- [ ] 5.6 Test all 8 workloads pass in compact+legacy modes
- [ ] 5.7 Achieve 95%+ coverage for all new code paths

## 6. Benchmarking
- [ ] 6.1 Run WL-001 through WL-008 after each phase
- [ ] 6.2 Track cumulative ratio improvement per phase
- [ ] 6.3 Compare final ratios against OodleNetwork baseline
- [ ] 6.4 Verify no throughput regression (compress/decompress MB/s)

## 7. Documentation
- [ ] 7.1 Update README benchmark tables with new ratios
- [ ] 7.2 Update CHANGELOG with optimization entries
- [ ] 7.3 Update RFC-001 if dict format changes (v5)
