## 1. Core Utilities
- [ ] 1.1 Implement CRC32 (table-based, SIMD-ready interface)
- [ ] 1.2 Implement bitstream writer (LSB-first, 64-bit accumulator)
- [ ] 1.3 Implement bitstream reader
- [ ] 1.4 Write tests for CRC32 and bitstream (test_bitstream.c)

## 2. Dictionary Training
- [ ] 2.1 Implement byte frequency counting from corpus
- [ ] 2.2 Implement ANS probability normalization (power-of-2 total)
- [ ] 2.3 Implement netc_dict_train()
- [ ] 2.4 Implement netc_dict_save() / netc_dict_load() with CRC32 validation
- [ ] 2.5 Write tests for dictionary training and serialization (test_dict.c)

## 3. rANS Codec (packets > 64 bytes)
- [ ] 3.1 Implement rANS encoder (state normalization, symbol encoding)
- [ ] 3.2 Implement rANS decoder (symbol lookup table, state reconstruction)
- [ ] 3.3 Integrate rANS into netc_compress / netc_decompress
- [ ] 3.4 Write tests: round-trip all 8 benchmark workloads (test_compress.c)

## 4. tANS Codec (packets <= 64 bytes)
- [ ] 4.1 Implement tANS 12-bit decode table builder
- [ ] 4.2 Implement tANS encoder
- [ ] 4.3 Implement tANS decoder (branch-free lookup)
- [ ] 4.4 Write tests: small packet round-trip (test_compress.c)

## 5. Compression Pipeline Integration
- [ ] 5.1 Implement algorithm selection logic (tANS vs rANS vs passthrough)
- [ ] 5.2 Implement compressed packet header write/read
- [ ] 5.3 Verify passthrough still activates when ANS expands data
- [ ] 5.4 Verify 95%+ test coverage

## 6. Validation
- [ ] 6.1 Round-trip test: 100,000 random packets per workload
- [ ] 6.2 Verify compression ratio ≤ 0.55 on WL-001 with trained dict
- [ ] 6.3 Update docs/design/algorithm-decisions.md
