## ADDED Requirements

### Requirement: SIMD Level Accessor
The library SHALL expose a `netc_ctx_simd_level()` function that returns the
actual hardware SIMD level selected at runtime (GENERIC, SSE42, AVX2, or NEON),
independent of the `simd_level` field in `netc_cfg_t`.

#### Scenario: Auto-detect reports actual level
Given a context created with `cfg.simd_level = 0` (auto) on an AVX2-capable machine
When `netc_ctx_simd_level(ctx)` is called
Then it returns `NETC_SIMD_LEVEL_AVX2`

#### Scenario: Forced generic reports generic
Given a context created with `cfg.simd_level = NETC_SIMD_LEVEL_GENERIC`
When `netc_ctx_simd_level(ctx)` is called
Then it returns `NETC_SIMD_LEVEL_GENERIC`

## MODIFIED Requirements

### Requirement: Bench SIMD Label
The benchmark output SHALL display the actual detected SIMD level string
(`generic`, `sse42`, `avx2`, or `neon`) in the compressor name column,
not the numeric configured value.

#### Scenario: Bench shows detected level
Given bench runs on an AVX2-capable machine with default `--simd=auto`
When results are printed
Then the netc row label contains `simd=avx2`

### Requirement: Dictionary Training Frequency Counting
The `netc_dict_train()` function SHALL use the SIMD-accelerated frequency
counting path (`netc_simd_ops_t.freq_count`) for unigram accumulation when
AVX2 or SSE4.2 is available, producing identical frequency tables as the
scalar path.

#### Scenario: SIMD freq_count matches scalar
Given a training corpus of 1000 packets
When frequency tables are built via SIMD freq_count on an AVX2 machine
Then all 16 bucket × 256 frequency entries are identical to scalar output

#### Scenario: Fallback on generic hardware
Given a machine without SSE4.2 or AVX2
When `netc_dict_train()` runs
Then the generic scalar `freq_count` path is used with no error
