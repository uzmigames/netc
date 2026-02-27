## 1. Decompressor Safety Hardening
- [x] 1.1 Add output size cap enforcement (dst_cap strictly respected, never exceeded)
- [x] 1.2 Add ANS state bounds check on every symbol decode iteration
- [x] 1.3 Add input bounds check (never read beyond src_size)
- [x] 1.4 Add original_size validation (reject if > NETC_MAX_PACKET_SIZE or > dst_cap)
- [x] 1.5 Add dictionary checksum verification on every load (not just first load)
- [x] 1.6 Write tests: all safety checks trigger NETC_ERR_CORRUPT on malformed input

## 2. Fuzz Testing
- [x] 2.1 Write libFuzzer target: fuzz_decompress (arbitrary bytes as compressed input)
- [x] 2.2 Write libFuzzer target: fuzz_dict_load (arbitrary bytes as dictionary)
- [x] 2.3 Write libFuzzer target: fuzz_compress (arbitrary packet content)
- [ ] 2.4 Run fuzz targets for minimum 10,000,000 iterations in CI
- [ ] 2.5 Document any crashes found and verify they are fixed

## 3. Sanitizer Testing
- [ ] 3.1 Build with -fsanitize=address,undefined and run full test suite
- [ ] 3.2 Build with -fsanitize=memory (clang) and run full test suite
- [ ] 3.3 Verify zero sanitizer errors on all workloads

## 4. Coverage Enforcement
- [x] 4.1 Add gcov/llvm-cov CMake target (make coverage)
- [ ] 4.2 Add coverage gate in CI (fail if < 95% line coverage)
- [ ] 4.3 Add tests for any uncovered branches until ≥ 95% achieved
- [ ] 4.4 Generate HTML coverage report artifact in CI

## 5. PGO Build Support
- [x] 5.1 Add CMake PGO build targets (instrument, profile-run, optimized)
- [ ] 5.2 Document PGO build workflow in docs/
- [ ] 5.3 Benchmark: verify PGO build ≥ 5% faster than non-PGO on WL-001

## 6. Release Documentation
- [x] 6.1 Finalize README.md (installation, quick start, API overview, benchmarks table)
- [x] 6.2 Write CHANGELOG.md (v0.1.0 release notes)
- [x] 6.3 Write docs/api-reference.md (all public functions documented)
- [ ] 6.4 Verify all RFC-002 CI gates pass (./bench --ci-check)
- [ ] 6.5 Tag v0.1.0 release
