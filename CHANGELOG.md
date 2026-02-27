# Changelog

All notable changes to **netc** are documented in this file.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html)

---

## [Unreleased]

- Zstd `ZDICT_trainFromBuffer` dependency added to bench (optional, auto-detected)

---

## [0.1.0] — 2026-02-27

First complete release of netc — a C11 network packet compression library.

### Added

#### Core Library (`src/`)
- **tANS entropy codec** (`src/algo/netc_tans.c`) — 12-bit table (4096 states), branch-free lookup decode, FSE spread function with coprime step 2563, CRC32-protected table serialization (RFC-001 §5)
- **Delta prediction** (`src/algo/netc_delta.c`) — field-class aware inter-packet delta encoding; 20–40% ratio improvement on game/telemetry packets (AD-002)
- **Compression context** (`src/core/netc_ctx.c`) — stateful ring-buffer history (default 64 KB), zero hot-path allocation via pre-allocated arena (AD-005)
- **Decompressor** (`src/core/netc_decompress.c`) — security hardened: output cap enforcement, ANS state bounds checking, input bounds checking, `original_size` validation, model_id verification
- **Dictionary management** (`src/core/netc_dict.c`) — `netc_dict_train` / `netc_dict_save` / `netc_dict_load` with CRC32 integrity on every load
- **Passthrough guarantee** — incompressible payloads emitted verbatim with `NETC_PKT_FLAG_PASSTHRU`; output always ≤ `src_size + NETC_HEADER_SIZE`
- **Stateless API** — `netc_compress_stateless` / `netc_decompress_stateless` for UDP / out-of-order payloads
- **SIMD acceleration** (`src/simd/`) — SSE4.2 and AVX2 (x86), ARM NEON, runtime dispatch, identical output across paths (Phase 4)

#### Public API (`include/netc.h`)
- Single-header public API, C11, no external dependencies
- `netc_ctx_create` / `netc_ctx_destroy` / `netc_ctx_reset` / `netc_ctx_stats`
- `netc_dict_train` / `netc_dict_save` / `netc_dict_load` / `netc_dict_free` / `netc_dict_free_blob` / `netc_dict_model_id`
- `netc_compress` / `netc_decompress`
- `netc_compress_stateless` / `netc_decompress_stateless`
- `netc_compress_bound` / `netc_strerror` / `netc_version`

#### Benchmark Harness (`bench/`)
- 8 RFC-002 workload generators (WL-001..WL-008): game state, financial tick, telemetry, random, repetitive, mixed
- Timer infrastructure: RDTSC (x86), `clock_gettime(CLOCK_MONOTONIC_RAW)` (Linux/macOS), `QueryPerformanceCounter` (Windows)
- Percentile calculator: p50, p90, p99, p999, mean, stddev
- Output reporters: table, CSV (RFC-002 §7.3), JSON (RFC-002 §7.4)
- CI gate checker: PERF-01..06, RATIO-01..02, SAFETY-01, MEM-01 (RFC-002 §6)
- Compressor adapters: netc (stateful+dict+delta), zlib (level=1/6), LZ4 (fast/HC), Zstd (level=1/3, level=1+dict) — optional, auto-detected by CMake
- COMP-* CI gates: netc must out-compress each available competitor on WL-001
- Benchmark README with usage examples and OodleNetwork comparison instructions

#### Tests (`tests/`)
- Unit tests with Unity C framework: `test_api`, `test_passthru`, `test_bitstream`, `test_dict`, `test_compress`, `test_tans_debug`, `test_delta`, `test_simd`
- Security test suite (`test_security.c`) — 28 tests covering: output cap enforcement, ANS state bounds, truncated input, `original_size` validation, dict checksum, NULL guards, unknown algorithm, model_id mismatch, crash safety
- libFuzzer targets: `fuzz_decompress`, `fuzz_dict_load`, `fuzz_compress`

#### Build System (`CMakeLists.txt`)
- C11, strict `-Wall -Wextra -Wpedantic -Werror` / MSVC `/W4 /WX`
- ASan + UBSan enabled by default in Debug builds (non-MSVC)
- `NETC_BUILD_FUZZ=ON` — builds libFuzzer targets (requires clang)
- `NETC_ENABLE_COVERAGE=ON` — gcov/llvm-cov instrumentation + lcov/genhtml `coverage` target
- `NETC_PGO_INSTRUMENT=ON` / `NETC_PGO_OPTIMIZE=ON` — three-step PGO workflow
- `NETC_BENCH_WITH_OODLE=ON` — OodleNetwork adapter (requires `UE5_OODLE_SDK` path)
- Shared library (`netc_shared`) for C# P/Invoke

#### CI (`.github/workflows/ci.yml`)
- Linux (GCC + Clang, Release + Debug), Windows (MSVC, Release + Debug), macOS (Apple Clang ARM64)
- ASan + UBSan enabled on all Debug matrix entries
- cppcheck static analysis job
- Benchmark CI job: builds with zlib/LZ4/Zstd, runs all workloads, uploads `bench_results.json` artifact

#### Documentation
- `docs/rfc/RFC-001-netc-compression-protocol.md` — wire format, packet header, API specification, security model
- `docs/rfc/RFC-002-benchmark-performance-requirements.md` — performance targets, workload definitions, CI gate criteria, measurement methodology
- `docs/api-reference.md` — complete public API reference with examples
- `docs/design/algorithm-decisions.md` — architecture decision log (ANS choice, delta prediction, SIMD, C11, memory model)
- `bench/README.md` — benchmark harness usage, CLI reference, OodleNetwork comparison guide
- `README.md` — project overview, quick start, performance table, SDK examples

### Performance Targets (v0.1.0)

| Gate | Target |
|------|--------|
| PERF-01 | Compress ≥ 2 GB/s (WL-001, single core) |
| PERF-02 | Decompress ≥ 4 GB/s (WL-001) |
| PERF-03 | Compress p99 ≤ 1 µs (WL-002, 128B) |
| PERF-04 | Decompress p99 ≤ 500 ns (WL-002, 128B) |
| PERF-05 | Compress ≥ 5 Mpps (WL-001, 64B) |
| PERF-06 | Decompress ≥ 10 Mpps (WL-001, 64B) |
| RATIO-01 | Ratio ≤ 0.55 (WL-001, trained dict) |
| RATIO-02 | Ratio ≤ 1.01 (WL-006, random passthrough) |

### Security

- Decompressor enforces `dst_cap` strictly — never writes beyond caller-supplied buffer
- ANS state validated on every decode iteration — corrupt state returns `NETC_ERR_CORRUPT`
- Input bounds checked — no reads beyond `src_size`
- `original_size` validated against `NETC_MAX_PACKET_SIZE` and `dst_cap` before decoding
- Dictionary CRC32 validated on every `netc_dict_load` call
- libFuzzer targets cover all three entry points: decompress, dict_load, compress

### Known Limitations

- rANS codec deferred to v0.2 (tANS used for all packet sizes in v0.1)
- Zstd dictionary training requires `ZDICT_trainFromBuffer` — the bench adapter falls back to no-dict if training fails
- OodleNetwork adapter requires manual UE5 SDK path (`-DUE5_OODLE_SDK=...`)
- MSVC: coverage (`NETC_ENABLE_COVERAGE`) not supported; use clang or GCC

---

[Unreleased]: https://github.com/your-org/netc/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/your-org/netc/releases/tag/v0.1.0
