## 1. Project Structure
- [x] 1.1 Create directory tree (src/core, src/algo, src/simd, src/util, include, bench, tests)
- [x] 1.2 Write include/netc.h (complete public API per RFC-001 §10)
- [x] 1.3 Write CMakeLists.txt with O3, SIMD detection, debug/release targets
- [x] 1.4 Write Makefile wrapper

## 2. Passthrough Baseline
- [x] 2.1 Implement netc_compress (passthrough mode, NETC_PKT_FLAG_PASSTHRU)
- [x] 2.2 Implement netc_decompress (passthrough mode)
- [x] 2.3 Implement netc_ctx_create / netc_ctx_destroy / netc_ctx_reset
- [x] 2.4 Implement netc_strerror

## 3. Test Framework
- [x] 3.1 Integrate Unity C test framework (v2.6.0, vendor/unity/)
- [x] 3.2 Write test_api.c (API contract tests, all return codes)
- [x] 3.3 Write test_passthru.c (round-trip passthrough tests)
- [x] 3.4 Verify build and tests pass (100% — 2/2 test targets, all assertions passing)

## 4. CI Pipeline
- [x] 4.1 GitHub Actions: build + test on Linux (GCC + Clang), Windows (MSVC), macOS
- [x] 4.2 GitHub Actions: lint check (cppcheck)
- [ ] 4.3 Verify CI passes (pending first push)

## 5. Documentation
- [ ] 5.1 Write README.md (project overview, build instructions, quick start)
