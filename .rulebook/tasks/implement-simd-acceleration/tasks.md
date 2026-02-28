## 1. SIMD Detection
- [x] 1.1 Implement x86 CPUID detection (SSE4.2 leaf 1 ECX bit 20; AVX2 leaf 7 EBX bit 5 + OSXSAVE + XGETBV)
- [x] 1.2 Implement ARM NEON detection (__ARM_NEON / __aarch64__ — mandatory on AArch64)
- [x] 1.3 Implement dispatch table (netc_simd_ops_t with function pointers selected at ctx_create)
- [x] 1.4 Write tests: detect() returns valid level; dispatch table level field matches selected level

## 2. Generic Fallback (C11, no intrinsics)
- [x] 2.1 Implement netc_delta_encode_generic / netc_delta_decode_generic (field-class aware)
- [x] 2.2 Implement netc_freq_count_generic (byte frequency histogram, ADDS to existing counts)
- [x] 2.3 Implement netc_crc32_update_generic (table-based ISO-HDLC CRC32, standard "123456789" vector)
- [x] 2.4 Tests: generic roundtrip, freq_count accumulation, CRC32 known values

## 3. SSE4.2 Implementation
- [x] 3.1 Implement netc_crc32_update_sse42 (hardware _mm_crc32_u8/u32/u64, CRC32C polynomial)
- [x] 3.2 Implement netc_delta_encode_sse42 (16 bytes/cycle _mm_sub_epi8 / _mm_xor_si128 per region)
- [x] 3.3 Implement netc_delta_decode_sse42 (16 bytes/cycle _mm_add_epi8 / _mm_xor_si128)
- [x] 3.4 Tests: SSE4.2 encode/decode/freq output identical to generic for all field-class regions

## 4. AVX2 Implementation
- [x] 4.1 Implement netc_delta_encode_avx2 (32 bytes/cycle BODY+TAIL, 16 bytes/cycle SUBHEADER)
- [x] 4.2 Implement netc_delta_decode_avx2 (32 bytes/cycle _mm256_add_epi8 / _mm256_xor_si256)
- [x] 4.3 Implement netc_freq_count_avx2 (4-way partial histogram merge, 32 bytes/iter load)
- [x] 4.4 Tests: AVX2 output identical to generic at sizes 8, 17, 65, 257, 512 bytes

## 5. ARM NEON Implementation
- [ ] 5.1 Implement NEON delta encode (vsubq_u8) — deferred to ARM build
- [ ] 5.2 Implement NEON delta decode (vaddq_u8)
- [ ] 5.3 Implement NEON frequency counting
- [ ] 5.4 Tests on ARM target

## 6. Benchmarks and Validation
- [x] 6.3 Verified output identical across generic, SSE4.2, AVX2 paths (22 tests, all passing)
- [ ] 6.1 Benchmark: verify AVX2 path achieves ≥ 3 GB/s on WL-001 (deferred: requires dedicated AVX2 server; current dev machine: 340 MB/s decompress)
- [ ] 6.2 Benchmark: verify Mpps (decompress, 64B) ≥ 20 with SIMD (deferred: current 5.4 Mpps on dev machine)
- [ ] 6.4 Verify 95%+ test coverage on simd module (deferred: requires gcov on Linux CI)
