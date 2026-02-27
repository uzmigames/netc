## 1. SIMD Detection
- [ ] 1.1 Implement x86 CPUID detection (SSE4.2, AVX2 capability bits)
- [ ] 1.2 Implement ARM AT_HWCAP NEON detection (Linux)
- [ ] 1.3 Implement dispatch table (function pointers selected at ctx_create)
- [ ] 1.4 Write tests: SIMD detection reports correct capabilities on current CPU

## 2. Generic Fallback (C11, no intrinsics)
- [ ] 2.1 Implement generic delta_encode_bulk / delta_decode_bulk
- [ ] 2.2 Implement generic freq_count (byte frequency histogram)
- [ ] 2.3 Implement generic crc32_update
- [ ] 2.4 Write tests: generic path produces identical output to scalar reference

## 3. SSE4.2 Implementation
- [ ] 3.1 Implement SSE4.2 CRC32 (hardware _mm_crc32_u8 / _mm_crc32_u32)
- [ ] 3.2 Implement SSE4.2 delta encode (16 bytes/cycle with psubusb / psubb)
- [ ] 3.3 Implement SSE4.2 delta decode (16 bytes/cycle)
- [ ] 3.4 Write tests: SSE4.2 output matches generic output for all workloads

## 4. AVX2 Implementation
- [ ] 4.1 Implement AVX2 delta encode (32 bytes/cycle with _mm256_sub_epi8)
- [ ] 4.2 Implement AVX2 delta decode (32 bytes/cycle)
- [ ] 4.3 Implement AVX2 byte frequency counting (histogram over 256-byte chunks)
- [ ] 4.4 Write tests: AVX2 output matches SSE4.2 output for all workloads

## 5. ARM NEON Implementation
- [ ] 5.1 Implement NEON delta encode (16 bytes/cycle with vsubq_u8)
- [ ] 5.2 Implement NEON delta decode (16 bytes/cycle)
- [ ] 5.3 Implement NEON frequency counting
- [ ] 5.4 Write tests: NEON output matches generic output

## 6. Benchmarks and Validation
- [ ] 6.1 Benchmark: verify AVX2 path achieves ≥ 3 GB/s on WL-001
- [ ] 6.2 Benchmark: verify Mpps (decompress, 64B) ≥ 20 with SIMD
- [ ] 6.3 Verify output identical across all SIMD paths (byte-for-byte)
- [ ] 6.4 Verify 95%+ test coverage on simd module
