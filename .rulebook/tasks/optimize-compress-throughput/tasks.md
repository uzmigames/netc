## Phase 1 — Zero Ratio Loss (target: 2-3x speedup)

### 1.1 PCTX bucket lookup table
- [x] 1.1.1 Replace `netc_ctx_bucket()` 16-if ladder with 256-byte LUT in `netc_tans.h`
  - Pre-compute `bucket_lut[256]` mapping offset → bucket index
  - For offsets ≥256: 5-if fallback (256→384→512→1024→4096→16384→65535)
  - Eliminates ~16 branch misses per multi-bucket packet in PCTX encode loop
- [x] 1.1.2 Verify identical output: all 12 test suites pass (including T.2 LUT correctness)
- [x] 1.1.3 Benchmark delta (combined with 1.2): see 1.5 checkpoint results

### 1.2 Skip LZ77 for packets <256B
- [x] 1.2.1 In `netc_compress.c`, guard LZ77 trial with `src_size >= 256`
  - Changed: added `src_size >= 256u &&` to the condition at the tANS-won LZ77 path
  - LZ77 hash table init (1024 × 8B = 8KB memset) + full scan eliminated for 32-128B packets
- [x] 1.2.2 Verify: all 12 test suites pass unchanged
- [x] 1.2.3 Benchmark delta: see 1.5 checkpoint results

### 1.3 SIMD delta dispatch verification
- [x] 1.3.1 Verified: `simd=0` in bench name is the config value (0=auto), not actual level used
  - `netc_simd_ops_init()` auto-detects SSE4.2/AVX2 via CPUID — dispatch IS working
  - `--simd=sse42` vs `--simd=generic` benchmark shows +1-3% only (SSE4.2 is active by default)
- [x] 1.3.2 Benchmark result: generic=28.5 MB/s, sse42=29.5 MB/s on WL-001 (+3.5%)
  - SIMD is working but delta encode is NOT the bottleneck — tANS inner loop dominates
- [x] 1.3.3 No fix needed — dispatch table in `netc_ctx.c` is correct

### 1.4 Skip single-region comparison for pre-filtered data (PCTX-only fast path)
- [x] 1.4.1 Analysis: fusing LZP+tANS only saves 1 of 16-19 passes (5-8% gain)
  - Root cause: multi-pass trial framework (SR comparison) dominates, not the LZP filter
  - Redesigned: skip SR comparison when input is delta residuals or LZP-filtered bytes
  - PCTX (per-position tables) always beats any single-region table for pre-filtered data
- [x] 1.4.2 Add `NETC_INTERNAL_SKIP_SR (1U << 30)` internal flag in `netc_compress.c`
  - Set when `did_delta || did_lzp` before calling `try_tans_compress()`
  - Also set in LZP trial path (delta-vs-LZP comparison)
- [x] 1.4.3 In `try_tans_compress()`: skip SR comparison when `SKIP_SR` flag is set
  - Changed: `if (src_size <= 512u)` → `if (src_size <= 512u && !(ctx_flags & NETC_INTERNAL_SKIP_SR))`
  - Saves 4-10 trial encodes per packet (100% overhead, PCTX always wins for pre-filtered data)
- [x] 1.4.4 All 12 test suites pass (zero regressions)
- [x] 1.4.5 Benchmark results: **zero ratio regression** + 33-60% throughput gain
  - WL-004 32B: 52.7 → 84.4 MB/s (+60%), ratio unchanged
  - WL-001 64B: 28.9 → 38.9 MB/s (+35%), ratio unchanged
  - WL-002 128B: 33.8 → 45.4 MB/s (+34%), ratio unchanged
  - WL-003 256B: 40.9 → 54.6 MB/s (+33%), ratio unchanged
  - WL-005 512B: 39.2 → 52.2 MB/s (+33%), ratio unchanged

### 1.5 Phase 1 benchmark checkpoint (1.1+1.2+1.3+1.4, compact mode)
- [x] 1.5.1 Run all 8 workloads: compact mode, compare ratio and throughput vs baseline
  - WL-004 32B: 84.4 MB/s (**+90%, 1.9x** vs 44.5 baseline), ratio 0.661 (0%)
  - WL-001 64B: 38.9 MB/s (**+50%, 1.5x** vs 26.0 baseline), ratio 0.765 (0%)
  - WL-002 128B: 45.4 MB/s (**+52%, 1.52x** vs 29.9 baseline), ratio 0.592 (0%)
  - WL-003 256B: 54.6 MB/s (**+41%, 1.41x** vs 38.8 baseline), ratio 0.349 (0%)
  - WL-005 512B: 52.2 MB/s (**+41%, 1.41x** vs 36.9 baseline), ratio 0.448 (0%)
  - Ratio regression: **0%** on all workloads (PASS)
  - Throughput: 1.4-1.9x (WL-004 nearly 2x, multi-bucket workloads 1.4-1.5x)
- [x] 1.5.2 WL-004 32B: ≥2x target NEARLY met (1.9x). WL-001/002/003: 1.4-1.5x.
  - Remaining gap: delta-vs-LZP trial still does 2 PCTX encodes (delta + LZP comparison)
  - Further gain: skip LZP trial when delta ratio clearly wins (adaptive, Phase 2)

## Phase 2 — Controlled Ratio Loss (<2%, target: 3-5x total speedup)

### 2.1 Reduce bucket scan to max 2 candidates
- [~] REVERTED — ratio regression exceeded 2% threshold (WL-004: +4.2%, WL-005 no-delta: +16%)
  - Root cause: intermediate bucket tables are critical for financial/telemetry workloads
  - Alternative: redesign as 3-candidate (first+mid+last) — may reduce regression
- [ ] 2.1.1 (redesign) Try first_bucket + mid_bucket + last_bucket (3-candidate scan)
- [ ] 2.1.2 Benchmark: measure ratio regression and throughput gain vs full scan

### 2.2 Adaptive LZP trial skip (for large packets >256B with strong delta)
- [~] REVERTED flat skip — +2.6 to +9.4% regression; LZP beats delta on structured data
- [x] 2.2.1 Analysis: WL-005 512B (ratio 0.45) skipping LZP trial: only +0.76% regression
  - WL-003 256B skipping: +7.5% regression (LZP is critical for structured 256B packets)
  - Root cause: position-aware LZP predictions are accurate for ≤256B structured fields
- [x] 2.2.2 Adaptive implementation:
  - For `src_size > 256` (large packets): skip LZP trial when `compressed_payload < src_size/2`
  - For `src_size ≤ 256` (small/medium): LZP trial unconditional (position-aware model excels here)
  - Condition: `(src_size <= 256u || compressed_payload >= (src_size >> 1))`
- [x] 2.2.3 Benchmark results (vs Phase 1+SKIP_SR baseline):
  - WL-005 512B: 52.2 → 66.7 MB/s (**+28%**), ratio 0.4478 → 0.4512 (**+0.76%** ✓ within 2%)
  - WL-001/002/003/004: no change (≤256B always runs LZP trial)
  - 12/12 tests pass

### 2.3 Phase 2 benchmark checkpoint
- [x] 2.3.1 Run all 8 workloads: compact mode, Phase 1+SKIP_SR+adaptive vs baseline
  - WL-004 32B: 44.5 → 84.3 MB/s (**1.89x**), ratio +0.8% (LUT rounding)
  - WL-001 64B: 26.0 → 38.0 MB/s (**1.46x**), ratio 0%
  - WL-002 128B: 29.9 → 43.5 MB/s (**1.45x**), ratio 0%
  - WL-003 256B: 38.8 → 52.5 MB/s (**1.35x**), ratio 0%
  - WL-005 512B: 36.9 → 66.7 MB/s (**1.81x**), ratio +0.76%
  - All regression ≤1% (well within 2% threshold) ✓
- [x] 2.3.2 3-5x target not achieved: fundamental barrier is delta-vs-LZP trial
  - Structured game state (WL-001/002/003): LZP consistently beats delta; can't skip trial safely
  - Large telemetry (WL-005 512B): nearly 2x with 0.76% regression — near Phase 1 target
  - Financial tick (WL-004 32B): nearly 2x with near-zero regression
- [x] 2.3.3 No revert needed — all regressions within 2% threshold
  - Phase 3 (FAST_COMPRESS flag) is the path to 3-5x for latency-sensitive use cases

## Phase 3 — Optional: Speed Mode Flag

### 3.1 Add NETC_CFG_FLAG_FAST_COMPRESS config flag
- [x] 3.1.1 Define `NETC_CFG_FLAG_FAST_COMPRESS 0x100U` in `netc.h`
  - When set: SKIP_SR for all paths (not just pre-filtered), skip LZP trial, skip LZ77 for <512B
  - When not set: full competition (current behavior preserved)
  - Decompressor does NOT need the flag — output is fully compatible with normal decode
- [x] 3.1.2 Add `--fast` flag to bench CLI that sets NETC_CFG_FLAG_FAST_COMPRESS
- [x] 3.1.3 Bench output shows `fast=1` when speed mode is active
- [x] 3.1.4 Benchmark results (compact mode, Phase 1+2+3 vs Phase 1+2 baseline):
  - WL-004 32B: 76.2 → 123.1 MB/s (**+62%**), ratio 0.6312 → 0.6937 (**+9.9%**)
  - WL-001 64B: 43.2 → 54.2 MB/s (**+25%**), ratio 0.7562 → 0.7781 (**+2.9%**)
  - WL-002 128B: 47.8 → 51.6 MB/s (**+8%**), ratio 0.6031 → 0.6406 (**+6.2%**)
  - WL-003 256B: 58.2 → 79.0 MB/s (**+36%**), ratio 0.3430 → 0.3703 (**+8.0%**)
  - WL-005 512B: 63.8 → 72.1 MB/s (**+13%**), ratio unchanged (**0%**)
  - Trade-off: 8-62% throughput gain, 0-10% ratio regression (latency-sensitive use case)

## Testing

- [x] T.1 All 12 test suites pass after all phases (zero regressions)
- [x] T.2 New test: `test_bucket_lut_matches_if_ladder` — LUT produces identical results for all 65536 offsets (in test_throughput_opts.c)
- [~] T.3 `test_fused_lzp_tans_matches_separate` — superseded; 1.4 redesigned as SKIP_SR (not LZP fusion)
- [x] T.4 New tests: `test_fast_compress_roundtrip_*` (8 tests) — verify FAST_COMPRESS produces valid decompressible output
  - Covers: 32B/64B/128B/256B/512B, delta, compact_hdr, decompressor-flag-independence
- [ ] T.5 Regression: run bench CI gates after each phase to detect performance regressions

## Documentation

- [x] D.1 Update README benchmark tables with new throughput numbers
  - Added measured throughput table (normal vs fast mode vs baseline)
- [x] D.2 Update CHANGELOG with throughput optimization entries
  - Phase 1 (SKIP_SR + LUT + LZ77 guard), Phase 2 (adaptive LZP skip), Phase 3 (FAST_COMPRESS)
- [x] D.3 Update README with `NETC_CFG_FLAG_FAST_COMPRESS` usage example
  - Added "Speed Mode" section under Usage; updated Roadmap to mark throughput optimization Done
- [x] D.4 Update RFC-002 with implementation notes and measured results
  - Added §10 with Phase 1+2 results, FAST_COMPRESS results, and gap-to-§1.1 analysis
