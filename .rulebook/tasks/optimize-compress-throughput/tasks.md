## Phase 1 — Zero Ratio Loss (target: 2-3x speedup)

### 1.1 PCTX bucket lookup table
- [ ] 1.1.1 Replace `netc_ctx_bucket()` 16-if ladder with 256-byte LUT in `netc_tans.h`
  - Pre-compute `bucket_lut[256]` mapping offset → bucket index
  - For offsets ≥256: 3-if fallback (256→384→512→1024→4096→16384→65535)
  - Eliminates ~16 branch misses per multi-bucket packet in PCTX encode loop
- [ ] 1.1.2 Verify identical output: all 11 test suites must pass unchanged
- [ ] 1.1.3 Benchmark delta: expect 1.2-1.5x on PCTX workloads

### 1.2 Skip LZ77 for packets <256B
- [ ] 1.2.1 In `netc_compress.c`, guard LZ77 trial with `src_size >= 256`
  - Currently: LZ77 trial runs whenever `compressed_payload * 2 > src_size` (ratio > 0.5)
  - Change: add `&& src_size >= 256` to the condition
  - LZ77 hash table init (1024 × 8B = 8KB memset) + full scan is wasted on 64-128B packets
- [ ] 1.2.2 Verify: test_compress round-trips still pass
- [ ] 1.2.3 Benchmark delta: expect 1.1-1.3x on WL-001/WL-002, ~0% ratio loss

### 1.3 SIMD delta dispatch verification
- [ ] 1.3.1 Verify bench hot path uses `ctx->simd_ops.delta_encode` (SSE4.2/AVX2) not generic scalar
  - Check: is `simd=0` in bench output because SIMD dispatch is off or because bench forces it?
  - If forced generic: wire auto-detect so bench uses HW delta encode/decode
- [ ] 1.3.2 Benchmark with `--simd=auto` vs `--simd=generic` — measure delta encode speedup
- [ ] 1.3.3 If SSE4.2 delta not dispatched: fix dispatch table in `netc_ctx.c`

### 1.4 Fuse LZP + tANS into single pass
- [ ] 1.4.1 Add `netc_tans_encode_lzp()` variant in `netc_tans.c`
  - Takes `lzp_table` pointer, applies XOR prediction inline before encoding each byte
  - Eliminates intermediate arena buffer copy and second loop over packet data
  - Single loop: for each byte i, compute `lzp_predict(prev, i)`, XOR, then tANS encode
- [ ] 1.4.2 Add fallback: if `lzp_table == NULL`, behave identically to `netc_tans_encode()`
- [ ] 1.4.3 Update `netc_compress.c` to call fused version when LZP is active
- [ ] 1.4.4 Verify identical compressed output (bitstream must match non-fused path)
- [ ] 1.4.5 Benchmark delta: expect 1.2-1.4x on LZP-enabled workloads

### 1.5 Phase 1 benchmark checkpoint
- [ ] 1.5.1 Run all 8 workloads: compact + legacy, compare ratio and throughput vs baseline
- [ ] 1.5.2 Target: ≥2x compress throughput, 0% ratio regression

## Phase 2 — Controlled Ratio Loss (<2%, target: 3-5x total speedup)

### 2.1 Reduce bucket scan to max 2 candidates
- [ ] 2.1.1 In `netc_compress.c` multi-bucket path, only try:
  - Bucket of byte offset 0 (first_bucket)
  - Bucket of byte offset `src_size-1` (last_bucket)
  - Skip all intermediate buckets
- [ ] 2.1.2 If first_bucket == last_bucket: single trial (no scanning needed)
- [ ] 2.1.3 Remove the `for (b = first_bucket; b <= last_bucket; b++)` loop
- [ ] 2.1.4 Verify all tests pass
- [ ] 2.1.5 Benchmark: measure ratio regression (expect ≤0.5%) and throughput gain (expect 1.5-2x)

### 2.2 Skip delta-vs-LZP trial
- [ ] 2.2.1 In `netc_compress.c`, remove the LZP-only re-trial after delta+tANS succeeds
  - Currently: for packets ≤512B, after delta+tANS wins, the encoder re-runs LZP filter on raw bytes + tANS to see if LZP-only beats delta
  - Change: trust delta+tANS result, skip the re-trial
  - This eliminates 1 full LZP filter pass + 1 full tANS encode per delta packet
- [ ] 2.2.2 Verify all tests pass (some tests may exercise the delta-vs-LZP comparison path)
- [ ] 2.2.3 Benchmark: measure ratio regression (expect ≤1-2%) and throughput gain (expect 1.3-1.5x)

### 2.3 Phase 2 benchmark checkpoint
- [ ] 2.3.1 Run all 8 workloads: compact + legacy, compare ratio and throughput vs Phase 1
- [ ] 2.3.2 Target: ≥3x compress throughput from baseline, ≤2% total ratio regression
- [ ] 2.3.3 If ratio regression > 2%: revert 2.2 and keep Phase 1 only

## Phase 3 — Optional: Speed Mode Flag

### 3.1 Add NETC_CFG_FLAG_FAST_COMPRESS config flag
- [ ] 3.1.1 Define `NETC_CFG_FLAG_FAST_COMPRESS (1U << 8)` in `netc.h`
  - When set: skip bucket scanning (use first-byte bucket only), skip delta-vs-LZP trial, skip LZ77 for <512B
  - When not set: full competition (current behavior preserved)
- [ ] 3.1.2 Add `--fast` flag to bench CLI that sets NETC_CFG_FLAG_FAST_COMPRESS
- [ ] 3.1.3 Update bench output to show `fast=1` when speed mode is active

## Testing

- [ ] T.1 All 11 existing test suites pass after each phase (zero regressions)
- [ ] T.2 New test: `test_bucket_lut_matches_if_ladder` — verify LUT produces identical results for all 65536 offsets
- [ ] T.3 New test: `test_fused_lzp_tans_matches_separate` — verify fused LZP+tANS produces identical bitstream
- [ ] T.4 New test: `test_fast_compress_roundtrip` — verify FAST_COMPRESS flag produces valid decompressible output
- [ ] T.5 Regression: run bench CI gates after each phase to detect performance regressions

## Documentation

- [ ] D.1 Update README benchmark tables with new throughput numbers
- [ ] D.2 Update CHANGELOG with throughput optimization entries
- [ ] D.3 Update README with `NETC_CFG_FLAG_FAST_COMPRESS` usage example
- [ ] D.4 Update RFC-002 throughput targets if goals are met
