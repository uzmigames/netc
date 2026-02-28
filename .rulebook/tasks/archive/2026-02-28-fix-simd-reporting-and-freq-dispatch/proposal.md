# Proposal: fix-simd-reporting-and-freq-dispatch

## Why

Two SIMD gaps discovered during code audit:

1. **Display bug**: `bench` reports `simd=0` for all netc runs because
   `bench_netc.c` prints `simd_level` from `netc_cfg_t` (0 = auto) instead
   of `ctx->simd_ops.level` (actual detected level). Delta encode/decode ARE
   running SSE4.2/AVX2 correctly — the output just misleads users into
   thinking SIMD is disabled.

2. **Dormant freq_count dispatch**: `netc_simd_ops_init()` fills the
   `ops->freq_count` slot with SSE4.2/AVX2/NEON/generic implementations, but
   `netc_dict_train()` ignores the dispatch table and uses a manual scalar
   loop. The SIMD frequency counting code is already written — it just needs
   to be called.

## What Changes

- `bench/bench_netc.c`: print the actual detected SIMD level from
  `simd_ops.level` instead of the configured `simd_level` value.
  Requires `netc_ctx_simd_level()` accessor or direct field exposure.
- `src/core/netc_dict.c`: replace the manual scalar unigram frequency
  accumulation loop (lines 182-187) with a call to `simd_ops.freq_count()`.
  Training initializes a local `netc_simd_ops_t` (auto-detect, no context).

## Impact

- Affected specs: none (no wire format change)
- Affected code: `bench/bench_netc.c`, `src/core/netc_dict.c`, `include/netc.h`
- Breaking change: NO
- User benefit: bench output shows real SIMD level (e.g. `simd=avx2`);
  dictionary training 3-4x faster on large corpora with AVX2
