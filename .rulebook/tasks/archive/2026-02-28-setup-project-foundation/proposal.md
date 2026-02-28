# Proposal: Setup Project Foundation

## Why

netc requires a complete project skeleton before any algorithm implementation can begin. The foundation defines the build system, public API contract, directory structure, CI pipeline, and test framework that all subsequent phases depend on. Without this, developers cannot build or test the library and the 95% coverage target cannot be tracked.

## What Changes

- Create complete directory structure (src/core/, src/algo/, src/simd/, src/util/, include/, bench/, tests/, docs/)
- Write `include/netc.h` — complete public API header (types, functions, error codes, per RFC-001 §10)
- Implement passthrough compression baseline (establishes API contract, zero-compression baseline)
- CMake build system with -O3, SIMD detection, debug/release configs, PGO support
- Unity C test framework integration
- GitHub Actions CI pipeline (build, test, lint)
- Test scaffolding for all future test files

## Impact

- Affected specs: core/spec.md (new)
- Affected code: entire project (new)
- Breaking change: NO (new project)
- User benefit: Establishes the buildable, testable foundation that all subsequent phases depend on
