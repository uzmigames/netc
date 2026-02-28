## 1. Dictionary: LZP Hash Table Training
- [x] 1.1 Define LZP constants in `netc_lzp.h`
  - `NETC_LZP_ORDER` = 1 (position-aware order-1 context)
  - `NETC_LZP_HT_BITS` = 17 (131072 entries)
  - `NETC_LZP_HT_SIZE` = (1 << NETC_LZP_HT_BITS)
  - LZP hash function: `hash(prev_byte, position)`, masked to HT_SIZE-1
- [x] 1.2 Define LZP table as `uint8_t *lzp_table` (131072 × 2 = 256KB)
  - Each entry: predicted byte value
  - Implemented as flat array indexed by hash
- [x] 1.3 Add `lzp_table` pointer to `netc_dict_t`
  - NULL when no LZP model is present (v3 compat)
  - Allocated as separate block (256KB), freed in `netc_dict_free()`
- [x] 1.4 Implement LZP training in `netc_dict_train()`
  - Uses Boyer-Moore majority vote (task 1.5 merged)
  - For each training packet, for each byte: hash(prev_byte, pos) → slot
  - Majority vote resolves collisions across training corpus
- [x] 1.5 Optimize LZP training memory: Boyer-Moore majority vote
  - Per slot: `{ candidate_byte, vote_count }` — ~384KB working set
  - Much more cache-friendly than full 256-way counter array

## 2. Dictionary: Serialization v4
- [x] 2.1 Bump `NETC_DICT_VERSION` from 3 to 4 in `netc_internal.h`
- [x] 2.2 Add `dict_flags` field to dict blob header
  - Bit 0: `DICT_FLAG_LZP` — LZP table is present in blob
- [x] 2.3 Update `netc_dict_save()` to serialize LZP table after bigram tables
  - Write LZP entries (131072 × 2 bytes)
  - CRC32 checksum covers LZP data
- [x] 2.4 Update `netc_dict_load()` to deserialize LZP table
  - v4 with LZP flag: allocate and load lzp_table
  - v3 or no LZP flag: lzp_table = NULL (backward compat)
- [x] 2.5 Update `netc_dict_free()` to free lzp_table
- [x] 2.6 Update blob size calculation for variable dict sizes

## 3. Compression: LZP XOR Pre-Filter (design evolved from original proposal)
- [x] 3.1 Add `NETC_ALG_LZP` algorithm constant to `netc.h`
  - Implemented as XOR pre-filter + tANS (not flag+literal as originally proposed)
  - XOR each byte with LZP prediction: correct predictions → 0x00
  - Concentrated distribution feeds tANS for better compression
- [x] 3.2 Implement `netc_lzp_xor_filter()` in `netc_lzp.h`
  - Position-aware order-1 context: hash(prev_byte, position)
  - XOR with prediction: matched bytes become 0x00
- [x] 3.3 LZP wire format: XOR-filtered bytes fed to tANS entropy coder
  - Compact header encodes LZP algorithm + bucket (0x70-0x8F, 0x90-0xAF)
- [x] 3.4 Add LZP competition to `netc_compress()` main path
  - Delta-vs-LZP comparison: tries both delta+tANS and LZP+tANS, uses smaller
  - For packets ≤ 512B when LZP table available
- [x] 3.5 Hybrid LZP+tANS implemented (not optional — this IS the design)
  - LZP XOR filter concentrates distribution → tANS achieves better entropy coding
  - 2-8% improvement on structured game packets

## 4. Decompression: LZP Inverse
- [x] 4.1 Add `NETC_ALG_LZP` case to `netc_decompress()` switch
- [x] 4.2 Implement `netc_lzp_reconstruct()` in `netc_lzp.h`
  - tANS decode → get XOR-filtered bytes → XOR with same LZP predictions to recover original
- [x] 4.3 Hybrid LZP+tANS decompression: tANS decode first, then LZP reconstruct

## 5. Testing
- [x] 5.1 Test LZP hash table training with known data (in test_compact_header.c)
- [x] 5.2 Test LZP predict + reconstruct round-trip (compress/decompress with LZP dict)
- [x] 5.3 Test dict v4 serialization round-trip (save/load with LZP table)
- [x] 5.4 Test LZP compression ratio improvement (benchmarks: WL-001 through WL-008)
- [x] 5.5 Test LZP with delta pre-pass (delta+LZP comparison pipeline)
- [x] 5.6 Test edge cases (compact mode BIGRAM mismatch, X2 mismatch, tANS-raw fallback)
- [x] 5.7 Coverage maintained at 95%+ for all LZP code paths
- [x] 5.8 LZP+BIGRAM compact type tests (32 new type codes 0x90-0xAF, encode/decode roundtrip)

## 6. Benchmarking
- [x] 6.1 Run WL-001 through WL-008 with LZP-enabled dict — all pass compact+legacy
- [x] 6.2 Measured ratios: WL-001=0.765, WL-002=0.591, WL-003=0.349 (compact mode)
- [x] 6.3 LZP training integrated into existing dict training pipeline
- [x] 6.4 Throughput within targets (26-51 MB/s compress, 86-145 MB/s decompress)
- [x] 6.5 LZP hash table fits in L2 cache (256KB)

## 7. Documentation
- [x] 7.1 README updated with LZP XOR pre-filter in algorithm pipeline
- [x] 7.2 CHANGELOG updated with LZP entries (Added, Fixed, Changed sections)
- [x] 7.3 API docs reflect LZP configuration (NETC_CFG_FLAG_COMPACT_HDR enables LZP path)
- [x] 7.4 CHANGELOG entry for LZP+BIGRAM compact types and bug fixes

## 8. Compact Mode Bug Fixes (discovered during implementation)
- [x] 8.1 Fixed LZP compact mode BIGRAM mismatch (0x70-0x8F can't encode BIGRAM)
- [x] 8.2 Fixed LZP compact mode X2 mismatch (added NETC_INTERNAL_NO_X2 flag)
- [x] 8.3 Fixed tANS-raw fallback LZP path (same BIGRAM/X2 issues)
- [x] 8.4 Added 32 LZP+BIGRAM compact types (0x90-0xAF) to support BIGRAM with LZP
