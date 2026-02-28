# Proposal: fix-crc32-simd-dispatch

## Why
The SIMD dispatch table (`netc_simd_ops_t`) includes a `crc32_update` function pointer that correctly selects SSE4.2 hardware CRC32C (`_mm_crc32_u64/u32/u8`) when available. However, no code in the project actually calls it. All CRC32 call sites use the software table-based `netc_crc32()` from `src/util/netc_crc32.c`, completely bypassing the SIMD acceleration that was already implemented.

On CPUs with SSE4.2 (virtually all modern x86), hardware CRC32 is ~10-20x faster than software table lookup. While the current CRC32 usage is limited to dictionary checksumming (not the per-packet hot path), wiring the dispatch correctly ensures consistency with the SIMD architecture and prepares the codebase for future per-packet integrity checks.

## What Changes
1. **Replace `netc_crc32()` calls in `netc_dict.c`** with `ctx->simd_ops.crc32_update()` where a context is available, or introduce a module-level SIMD ops instance for standalone dict operations (train/load/save).
2. **Alternatively, make `netc_crc32()` itself dispatch-aware** by using a global/singleton SIMD ops that is initialized once (simpler, fewer call-site changes).
3. **Remove the duplicate software CRC32 table** from `netc_simd_generic.c` — the generic fallback and `netc_crc32.c` both maintain independent 256-entry tables doing the same thing.

## Impact
- Affected specs: None (internal optimization, same CRC32 polynomial output)
- Affected code: `src/util/netc_crc32.c`, `src/util/netc_crc32.h`, `src/core/netc_dict.c`, `src/simd/netc_simd_generic.c`
- Breaking change: NO (API unchanged, CRC32C vs CRC32 polynomial mismatch must be resolved — see tasks)
- User benefit: Faster dictionary load/save/validation; consistent SIMD architecture; ready for future per-packet CRC
