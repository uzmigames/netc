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

## 3. tANS Codec (single codec for all packet sizes — AD-001, AD-003)
- [ ] 3.1 Implement tANS 12-bit decode table builder (4096 entries × 4B = 16KB, fits L1)
- [ ] 3.2 Implement tANS encoder (symbol spread, state normalization)
- [ ] 3.3 Implement tANS decoder (branch-free lookup: sym + nb_bits + next_state per slot)
- [ ] 3.4 Write tests: round-trip all 8 benchmark workloads (test_compress.c)

## 4. Compression Pipeline Integration
- [ ] 4.1 Implement algorithm selection logic (tANS vs passthrough; rANS deferred to v0.2)
- [ ] 4.2 Implement compressed packet header write/read (per RFC-001 §9)
- [ ] 4.3 Verify passthrough activates when tANS output ≥ original size
- [ ] 4.4 Verify 95%+ test coverage

## 5. Validation
- [ ] 5.1 Round-trip test: 100,000 random packets per workload
- [ ] 5.2 Verify compression ratio ≤ 0.55 on WL-001 with trained dict
- [ ] 5.3 Update docs/design/algorithm-decisions.md if benchmark reveals need for rANS
