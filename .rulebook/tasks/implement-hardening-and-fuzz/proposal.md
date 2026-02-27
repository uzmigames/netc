# Proposal: Security Hardening, Fuzz Testing, and Release Readiness

## Why

netc processes untrusted network data. A malicious peer can craft compressed packets designed to exploit the decompressor — causing buffer overflows, infinite loops, or denial-of-service via decompression bombs. Security hardening and fuzz testing are mandatory before any production deployment. This phase also ensures the 95% coverage threshold is met, PGO builds work, and all documentation is release-ready.

## What Changes

- Implement decompressor safety checks (RFC-001 §15): bounds validation on every ANS state transition, output cap enforcement, dictionary checksum verification
- Write libFuzzer target (tests/fuzz_decompress.c) — fuzzes decompressor against arbitrary inputs
- Write AFL-compatible fuzzer target
- Run Address Sanitizer + Undefined Behavior Sanitizer over full test suite
- Achieve ≥ 95% line coverage measured by gcov/llvm-cov
- Implement PGO build support in CMake
- Complete all documentation (README, API docs, migration guide)
- Create release checklist and CHANGELOG

## Impact

- Affected specs: security/spec.md (new), coverage/spec.md (new)
- Affected code: src/core/netc_decompress.c (safety checks), tests/fuzz_decompress.c (new), CMakeLists.txt (PGO target, fuzz target, sanitizer target)
- Breaking change: NO
- User benefit: Production-safe decompressor that cannot be exploited via crafted packets; 95%+ test coverage provides confidence in correctness
