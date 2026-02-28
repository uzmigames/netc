## 1. Core Utilities
- [x] 1.1 Implement CRC32 (table-based, SIMD-ready interface)
- [x] 1.2 Implement bitstream writer (LSB-first, 64-bit accumulator, sentinel flush)
- [x] 1.3 Implement bitstream reader (MSB-first accumulator, sentinel-based init)
- [x] 1.4 Write tests for CRC32 and bitstream (test_bitstream.c)

## 2. Dictionary Training
- [x] 2.1 Implement byte frequency counting from corpus
- [x] 2.2 Implement ANS probability normalization (power-of-2 total)
- [x] 2.3 Implement netc_dict_train()
- [x] 2.4 Implement netc_dict_save() / netc_dict_load() with CRC32 validation
- [x] 2.5 Write tests for dictionary training and serialization (test_dict.c)

## 3. tANS Codec (single codec for all packet sizes — AD-001, AD-003)
- [x] 3.1 Implement tANS 12-bit decode table builder (4096 entries × 4B = 16KB, fits L1)
      Uses FSE spread function (step=2563, coprime with TABLE_SIZE=4096) for global chain.
- [x] 3.2 Implement tANS encoder (FSE spread, state normalization, sentinel bitstream)
- [x] 3.3 Implement tANS decoder (branch-free lookup: sym + nb_bits + next_state per slot)
- [x] 3.4 Write tests: round-trip all 8 benchmark workloads (test_compress.c, test_tans_debug.c)

## 4. Compression Pipeline Integration
- [x] 4.1 Implement algorithm selection logic (tANS vs passthrough)
- [x] 4.2 Implement compressed packet header write/read (per RFC-001 §9)
- [x] 4.3 Verify passthrough activates when tANS output ≥ original size
- [x] 4.4 All 6 test suites pass (100%): test_api, test_passthru, test_bitstream,
          test_dict, test_compress, test_tans_debug

## 5. Validation
- [x] 5.1 Round-trip test: compress/decompress round-trips verified for all 8 workloads
- [ ] 5.2 Verify compression ratio ≤ 0.55 on WL-001 with trained dict (Phase 5)
- [ ] 5.3 Update docs/design/algorithm-decisions.md if benchmark reveals need for rANS
