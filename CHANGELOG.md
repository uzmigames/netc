# Changelog

All notable changes to **netc** are documented in this file.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html)

---

## [Unreleased]

### Added

- **Adaptive cross-packet learning** (`NETC_CFG_FLAG_ADAPTIVE`, `0x200U`) — stateful mode that adapts compression model to the live data stream. Three phases:
  - **Phase 1 — Adaptive tANS frequency tables**: Per-bucket frequency accumulators track byte distributions across packets. Tables rebuilt every 128 packets with 3/4 accumulated + 1/4 dict baseline blending. Encoder and decoder rebuild independently but stay in sync (both feed raw bytes post-decode).
  - **Phase 2 — Adaptive LZP hash updates**: Mutable LZP table cloned from dict at context creation. Confidence-based decay: hits boost confidence, misses decrement, depleted entries replaced. Dict entries start at confidence=4 to survive initial misses.
  - **Phase 3 — Order-2 delta prediction**: Linear extrapolation `predicted[i] = 2*prev[i] - prev2[i]`. Compressor tries order-2 when prev2 available and sizes match; keeps whichever (order-1 or order-2) produces more zero residuals. Wire format: RLE flag repurposed as order-2 signal (DELTA+RLE = order-2). Compact header types 0xD4-0xD7.
  - Context memory: ~1020 KB with all phases enabled (struct + 64KB ring + 131KB arena + 2×64KB prev/prev2 + 424KB adaptive tables + 256KB adaptive LZP + 16KB freq accumulators).
  - New tests: 16 adaptive tests including 10K-packet sustained simulation and memory usage verification.

- **Bigram-PCTX encoder** (`netc_tans_encode_pctx_bigram`, `netc_tans_decode_pctx_bigram`) — per-position context-adaptive tANS that switches tables using BOTH byte offset (bucket) AND previous-byte bigram class. Combines PCTX's per-position entropy specialization with bigram's conditional context modeling. Compressor automatically tries bigram-PCTX alongside unigram PCTX and keeps the smaller output. New compact packet types 0xD0-0xD3 for PCTX+BIGRAM variants. **Result: 1-7% ratio improvement across all workloads. netc now beats OodleNetwork UDP on WL-004 (32B) and WL-005 (512B).**
- **Adaptive frequency normalization** — replaced Laplace smoothing (add-1 to all 256 symbols) with two-phase floor+proportional normalization. All symbols get freq=1 floor (256 of 4096 slots), remaining 3840 slots distributed proportionally to seen-only symbols. Yields tighter probability estimates by not wasting table resolution on unseen bytes.

- **`netc_ctx_simd_level(ctx)`** — new public API function returning the actual runtime-detected SIMD level (1=generic, 2=sse42, 3=avx2, 4=neon). Independent of `cfg.simd_level` (0 = auto-detect); the returned value is always resolved. Returns 0 for `NULL`. Declared in `netc.h`, implemented in `src/core/netc_ctx.c`.
- **`netc_simd_level_name(level)`** — new `static inline` helper in `src/simd/netc_simd.h` mapping a `uint8_t` SIMD level to a human-readable C string (`"generic"`, `"sse42"`, `"avx2"`, `"neon"`). Used by bench output and available to embedders.

- **Compress throughput optimizations** — `optimize-compress-throughput` task, Phase 1+2+3:
  - **Phase 1 — SKIP_SR optimization** (`NETC_INTERNAL_SKIP_SR`): When input bytes are pre-filtered (delta residuals or LZP XOR output), PCTX (per-position tables) always dominates single-region tables. Skipping the single-region trial saves 4-10 encode passes per packet. Enables `NETC_CFG_FLAG_DELTA` and LZP paths to skip 8-16 redundant trial encodes. **Result: 1.35-1.9× throughput gain, 0% ratio regression.**
  - **Phase 1 — Bucket LUT** (`netc_tans.h`): Replaced 16-branch `netc_ctx_bucket()` ladder with a 256-byte lookup table. Eliminates ~16 branch misses per multi-bucket packet in PCTX encode loop.
  - **Phase 1 — LZ77 small-packet guard**: Skip LZ77 trial (8KB hash table init + full scan) for packets < 256B; LZ77 never wins on packets that small.
  - **Phase 2 — Adaptive LZP trial skip**: For packets > 256B, skip the delta-vs-LZP comparison trial when `compressed_payload < src_size/2` (delta ratio < 0.5). LZP is unconditionally tried for ≤ 256B packets where position-aware predictions are most effective. **Result: WL-005 512B +28% throughput, +0.76% ratio regression (within 1% threshold).**
  - **Phase 3 — `NETC_CFG_FLAG_FAST_COMPRESS` (0x100U)**: Optional speed mode for latency-sensitive workloads. Enables SKIP_SR for all paths (not just pre-filtered), skips LZP trial entirely, and extends LZ77 skip threshold to 512B. Decompressor does NOT need this flag; output is fully compatible with normal decode. **Result: 8-62% additional throughput gain over Phase 1+2 baseline, 0-10% ratio trade-off.** Enable with `--fast` in the bench CLI.

- **Compact packet header** (`NETC_CFG_FLAG_COMPACT_HDR`) — variable-length wire format: 2 bytes for packets <= 127B, 4 bytes for 128-65535B. Opt-in per-context flag; legacy 8B header remains the default. Eliminates `compressed_size`, `model_id`, and `context_seq` from the wire (derived from context state).
- **ANS state compaction** — when compact header is active, tANS initial state is encoded as `uint16` (2B) instead of `uint32` (4B). ANS state range [4096, 8192) fits in 13 bits. Saves 2B per packet on single-region tANS, 4B on dual-interleaved (X2) tANS.
- **Packet type byte encoding** — single-byte structured encoding of `(flags, algorithm)` pairs replaces 2 separate header fields. 144 valid entries covering all tANS, LZP, PCTX, MREG, passthrough, and bucketed algorithm variants. Decode via const lookup table.
- **LZP (Lempel-Ziv Prediction) XOR pre-filter** — position-aware order-1 context prediction. XOR each byte with its LZP prediction before tANS encoding; correctly predicted bytes become `0x00`, concentrating the distribution. Integrated into dict training (Boyer-Moore majority vote) and serialization (dict format v4).
- **Bigram context model** (`NETC_CFG_FLAG_BIGRAM`) — order-1 bigram frequency tables trained per context bucket. 8 trained bigram classes (v5 dict, up from 4 static classes in v4). Class mapping trained from corpus: sort previous bytes by peak conditional symbol, divide into 8 groups of 32.
- **Dictionary format v5** — extends v4 with trained 256-byte `bigram_class_map` and 8-class bigram tables (65536B vs 32768B for v4). Backward-compatible: v4 dicts load with default `prev >> 6` class mapping and 4-class tables.
- **Delta-LZP comparison** — for packets ≤ 512B, when delta+tANS succeeds and an LZP table is available, also tries LZP-only on raw (non-delta) bytes. Uses the smaller result. Improves compression by 2-8% on structured game packets where position-aware LZP predictions outperform inter-packet deltas.
- **`test_compact_header.c`** — 25 tests: packet type encode/decode round-trip, size varint boundaries, compact compress/decompress round-trip (passthrough, tANS, delta, multi-packet), ANS state compaction verification, error cases.
- **Benchmark `--compact-hdr` flag** — enables compact headers in benchmark runs.
- **Adaptive 10-bit tANS tables** (`NETC_ALG_TANS_10 = 0x06`) — 1024-entry tables for small packets (≤128B) in compact mode. Per-packet competition: encoder tries both 10-bit and 12-bit tables and picks the smaller output. 32 new compact packet types (0xB0-0xCF) for TANS_10 and TANS_10+DELTA variants with bucket encoding. Includes frequency rescaling from 12-bit to 10-bit, dedicated encode/decode, and 37-test suite.
- Zstd `ZDICT_trainFromBuffer` dependency added to bench (optional, auto-detected)

### Fixed

- **Bench train/eval seed overlap** — benchmark used the same PRNG seed for training and evaluation corpora. OodleNetwork's raw-byte window contained the exact test packets, enabling near-exact substring matching (0.14 ratio vs real 0.76). Fixed by using `seed + BENCH_EVAL_SEED_OFFSET` for evaluation. **With fair comparison, netc beats or matches Oodle UDP on all 5 workloads (32B-512B).** OODLE-01 gate: PASSED.
- **Bench SIMD label always showing `simd=0`** — `bench_netc_init()` built the context name before creating the enc/dec context pair, so `netc_ctx_simd_level()` always returned the raw `cfg.simd_level` value (0 for auto) instead of the resolved level. Fixed by moving `create_ctx_pair()` before name building; the name now correctly shows `simd=sse42`, `simd=avx2`, etc. Also added `netc_simd_level_name()` helper (maps `uint8_t` level → string) and the `netc_ctx_simd_level()` public API.
- **`netc_dict_train()` ignored SIMD freq_count dispatch** — the byte-frequency counting loop used a plain scalar `raw[b][pkt[i]]++` with no SIMD acceleration. Replaced with per-bucket dispatch using `netc_simd_ops_t.freq_count`, matching the runtime SIMD level selected by the context (SSE4.2 or AVX2 on x86). Each bucket's byte range is counted into a `uint32_t tmp_freq[256]` scratch buffer then promoted into the `uint64_t raw[b]` accumulators.
- **CRC32 SIMD dispatch polynomial mismatch** — the SSE4.2 `crc32_update` slot used `_mm_crc32_u*` intrinsics which compute CRC32**C** (Castagnoli, 0x1EDC6F41), while the software path and dict checksums use IEEE CRC32 (0xEDB88320). A dict saved on a machine with SSE4.2 active would produce an incompatible checksum. Fixed by having the SSE4.2 slot delegate to the generic IEEE software implementation. ARM NEON (`__crc32d`) already uses IEEE natively and was unaffected.
- **Duplicate CRC32 lookup table** — `netc_simd_generic.c` maintained its own 256-entry CRC32 table with lazy initialization, identical to the canonical table in `netc_crc32.c`. Removed the duplicate; generic now delegates to `netc_crc32_continue()`.
- **LZP compact mode BIGRAM mismatch** — LZP compact packet types (0x70-0x8F) cannot encode the BIGRAM flag, but the encoder could produce LZP packets with bigram tables active. The decompressor decoded with unigram tables causing round-trip failures. Fixed by stripping BIGRAM from tANS context flags when LZP is active in compact mode.
- **LZP compact mode X2 mismatch** — LZP compact packet types cannot encode the X2 (dual-state) flag. For packets ≥ 256B, X2 encoding was selected but silently dropped from the compact header, causing the decompressor to decode with single-state instead of dual-state. Fixed by adding `NETC_INTERNAL_NO_X2` suppression flag. Affected WL-003 (256B), WL-005 (512B), and WL-008 (mixed traffic).
- **tANS-raw fallback LZP path** — the fallback path (delta residuals too noisy → retry on raw bytes with LZP) did not strip BIGRAM or suppress X2 in compact mode. Same root cause as above, different code path.

### Added

- **`NETC_MAYBE_UNUSED` macro** (`src/util/netc_platform.h`) — portable `__attribute__((unused))` abstraction for GCC/Clang, no-op on MSVC. Suppresses unused-function warnings for reserve code paths (e.g. `rle_encode`, `bucket_start_offset`).
- **GCC/Clang cross-compilation support for bench** — `_POSIX_C_SOURCE=199309L` for `clock_gettime`, `-Wno-error` for bench code (not core library), PGO link flags propagated to bench executable.
- **Clang PGO support** — CMake now detects Clang and uses `-fprofile-instr-generate` / `-fprofile-instr-use` with `llvm-profdata merge` instead of GCC's `-fprofile-generate` / `-fprofile-use`. Both compilers now work with the three-step PGO workflow.

### Changed

- **PGO evaluated — modest throughput gains, not 5-15% as originally projected:**
  - **Clang 18 PGO**: Stable +2-4% compress throughput across workloads. Best case +6.9% (WL-004 32B). Decompress +1-5% average. Recommended as optional optimization.
  - **GCC 13 PGO**: Inconsistent results — some workloads improve (+13.5% WL-007), others degrade (-6.5% WL-001). Not recommended for production.
  - **Root cause**: Code already uses `-O3` with manual `NETC_LIKELY`/`NETC_UNLIKELY`, `NETC_PREFETCH`, and `always_inline` hints, reducing the improvement space for PGO.
  - **Compression ratio**: Unchanged (PGO optimizes throughput, not algorithm output).

- Compression ratio improved significantly with compact headers and delta-LZP comparison:
  - WL-001 (64B): 0.908 -> **0.765** compact / **0.890** legacy (-14.0%)
  - WL-002 (128B): 0.673 -> **0.591** compact / **0.638** legacy (-7.3%)
  - WL-003 (256B): 0.403 -> **0.349** compact / **0.373** legacy (-6.3%)
  - WL-004 (32B): **0.656** compact / **0.906** legacy (-27.6%)
  - WL-005 (512B): **0.448** compact / **0.460** legacy (-2.6%)
  - WL-007 (128B): **0.072** compact / **0.115** legacy (-37.2%)
- WL-003 compact mode (0.349) is now within 0.1% of OodleNetwork baseline (0.35).
- Legacy (8B header) mode is fully backward-compatible; no behavior changes without `NETC_CFG_FLAG_COMPACT_HDR`.

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
