## 1. Fix SIMD Level Reporting in Bench

- [ ] 1.1 Add `netc_ctx_simd_level()` accessor to `include/netc.h`
  - Returns `ctx->simd_ops.level` (detected level: GENERIC/SSE42/AVX2/NEON)
- [ ] 1.2 Add `netc_simd_level_name()` helper in `src/simd/netc_simd.h`
  - Maps level uint8 → string: `"generic"`, `"sse42"`, `"avx2"`, `"neon"`
- [ ] 1.3 Update `bench/bench_netc.c` name string to use detected level
  - Replace `simd=%u` with `simd=%s` using `netc_simd_level_name()`
  - Call `netc_ctx_simd_level(n->enc_ctx)` after context creation

## 2. Wire freq_count SIMD into Dictionary Training

- [ ] 2.1 In `netc_dict_train()` (`src/core/netc_dict.c`):
  - Before the unigram loop, initialize local `netc_simd_ops_t ops`
    via `netc_simd_ops_init(&ops, NETC_SIMD_LEVEL_AUTO)`
  - Replace the inner `raw[bucket][pkt[i]]++` scalar loop with
    `ops.freq_count(pkt, pkt_size, raw, NETC_CTX_COUNT)`
  - Verify the `freq_count` signature matches: `(data, size, freq_table, bucket_count)`

## 3. Testing

- [ ] 3.1 All 11 existing test suites pass unchanged
- [ ] 3.2 Verify bench output shows `simd=avx2` (or `simd=sse42`) on x86
- [ ] 3.3 Add test: `test_simd_freq_count_matches_scalar` in `tests/test_simd.c`
  - Feed identical packet data to scalar and SIMD freq_count paths
  - Assert all 16 bucket × 256 frequency entries match exactly

## 4. Documentation

- [ ] 4.1 Update CHANGELOG with display fix and training speedup entry
