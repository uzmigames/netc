## 1. Implementation

- [x] 1.1 Decide polynomial strategy: **Option (B) chosen — Keep IEEE CRC32 everywhere.** SSE4.2 `_mm_crc32_u*` computes CRC32C (Castagnoli), which is incompatible with IEEE CRC32 used in dict checksums. No HW IEEE CRC32 intrinsic exists on x86 (would need PCLMULQDQ/CLMUL). Since CRC32 is only used for dict checksumming (cold path), HW acceleration is unnecessary. ARM NEON `__crc32d` already computes IEEE natively.
- [x] 1.2 Implement chosen polynomial in both generic (software) and SSE4.2 (hardware) paths with identical output
  - `netc_crc32_update_sse42()` now delegates to `netc_crc32_update_generic()` — removed `_mm_crc32_u*` intrinsics
  - `netc_crc32_update_generic()` now delegates to `netc_crc32_continue()` from `netc_crc32.c` — single canonical IEEE implementation
  - ARM NEON `netc_crc32_update_neon()` uses `__crc32d` (HW IEEE CRC32) — already correct, unchanged
- [x] 1.3 Consolidate duplicate CRC table: removed `s_crc32_table[256]`, `s_crc32_table_init`, and `crc32_table_build()` from `netc_simd_generic.c`. Generic now calls through `netc_crc32_continue()` which uses the canonical table in `netc_crc32.c`.
- [x] 1.4 Wire CRC32 through SIMD dispatch: all dispatch paths (generic, SSE42, NEON) now produce identical IEEE CRC32 output. `netc_dict.c` uses `netc_crc32()` directly which is the same IEEE implementation — consistent by construction. No global dispatch needed since all paths are identical.
- [x] 1.5 Verify that `netc_dict_load()` correctly validates checksums produced by `netc_dict_train()` — confirmed via `test_dict_crc32_roundtrip` test (train→save→load round-trip with internal CRC32 validation).

## 2. Testing

- [x] 2.1 Existing test: CRC32 known-vector test (`"123456789"` → `0xCBF43926`) in `test_generic_crc32_abc` — passes.
- [x] 2.2 Added test `test_sse42_crc32_matches_generic`: verifies SSE4.2 and generic paths produce identical CRC for both known vector and random data — passes.
- [x] 2.3 Added test `test_dict_crc32_roundtrip`: dictionary train→save→load round-trip with internal CRC32 checksum validation — passes.
- [x] 2.4 All existing `test_simd.c` CRC32 tests still pass (10/10 tests on MSVC).
- [ ] 2.5 Verify coverage ≥ 95% for modified files (deferred — requires GCC/lcov, MSVC does not support coverage).

## 3. Documentation

- [x] 3.1 Updated CHANGELOG.md with CRC32 polynomial mismatch fix and duplicate table removal.
- [x] 3.2 Updated `netc_crc32.h` header comments to document that all SIMD dispatch paths produce IEEE CRC32, and explain why SSE4.2 delegates to generic.
- [x] 3.3 No polynomial change (kept IEEE CRC32) — no RFC-001 or dict version bump needed.
