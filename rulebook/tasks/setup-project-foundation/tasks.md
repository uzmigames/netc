## 1. Project Structure
- [ ] 1.1 Create directory tree (src/core, src/algo, src/simd, src/util, include, bench, tests)
- [ ] 1.2 Write include/netc.h (complete public API per RFC-001 §10)
- [ ] 1.3 Write CMakeLists.txt with O3, SIMD detection, debug/release targets
- [ ] 1.4 Write Makefile wrapper

## 2. Passthrough Baseline
- [ ] 2.1 Implement netc_compress (passthrough mode, NETC_PKT_FLAG_PASSTHRU)
- [ ] 2.2 Implement netc_decompress (passthrough mode)
- [ ] 2.3 Implement netc_ctx_create / netc_ctx_destroy / netc_ctx_reset
- [ ] 2.4 Implement netc_strerror

## 3. Test Framework
- [ ] 3.1 Integrate Unity C test framework
- [ ] 3.2 Write test_api.c (API contract tests, all return codes)
- [ ] 3.3 Write test_passthru.c (round-trip passthrough tests)
- [ ] 3.4 Verify build and tests pass

## 4. CI Pipeline
- [ ] 4.1 GitHub Actions: build + test on Linux and Windows
- [ ] 4.2 GitHub Actions: lint check (cppcheck)
- [ ] 4.3 Verify CI passes

## 5. Documentation
- [ ] 5.1 Write README.md (project overview, build instructions, quick start)
