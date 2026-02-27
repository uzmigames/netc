# Proposal: Implement SIMD Acceleration (SSE4.2, AVX2, NEON)

## Why

The scalar ANS implementation already meets the baseline 2 GB/s target, but SIMD acceleration is required to hit the soft targets (≥ 3 GB/s with AVX2, ≥ 20 Mpps decompress). Bulk operations in the compression pipeline — frequency counting, delta encoding, CRC32, memory copies — have high data-level parallelism that maps directly to SIMD vector units. Without SIMD, netc leaves 2–4× throughput on the table on modern CPUs that support AVX2 or NEON.

## What Changes

- Implement runtime SIMD capability detection (CPUID on x86, AT_HWCAP on ARM Linux)
- Implement SSE4.2 acceleration: CRC32 (hardware crc32 instruction), byte frequency counting, delta encode/decode
- Implement AVX2 acceleration: 256-bit delta encoding (32 bytes/cycle), bulk ANS normalization
- Implement ARM NEON acceleration: 128-bit delta, frequency counting
- Implement generic fallback path (C99 compatible, no intrinsics)
- Dispatch table selected at context creation (zero runtime overhead in hot path)

## Impact

- Affected specs: simd/spec.md (new)
- Affected code: src/simd/netc_simd_sse42.c (new), src/simd/netc_simd_avx2.c (new), src/simd/netc_simd_neon.c (new), src/simd/netc_simd_generic.c (new), src/core/netc_ctx.c (dispatch table init)
- Breaking change: NO (transparent to API, identical output regardless of SIMD path)
- User benefit: 2–4× throughput improvement on modern x86 and ARM hardware, enabling ≥ 20 Mpps decompression
