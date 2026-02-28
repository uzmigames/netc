# netc — Product Requirements Document (PRD)

```
Version:  0.1.0
Date:     2026-02-26
Status:   APPROVED FOR DEVELOPMENT
```

---

## 1. Executive Summary

**netc** is an open-source C library for compressing low-entropy binary network packets at wire speed. It targets high-frequency network scenarios (game servers, financial systems, telemetry) where millions of small packets per second must be compressed with sub-microsecond latency.

**Definition of Done**: netc is considered functionally complete only when its benchmark results — compression ratio and throughput — are at parity with **OodleNetwork 2.9.13** (RAD Game Tools) on the standard game packet workloads. Beating open-source compressors (zlib, LZ4, Zstd) is a necessary intermediate milestone, not the finish line. If reaching Oodle parity requires changing the planned implementation strategy, the strategy changes. The target does not.

---

## 2. Problem Statement

### 2.1 The Bottleneck

Modern game servers, matching systems, and real-time platforms send 1–10 million packets/second. At 128 bytes/packet:
- 10 Mpps × 128 bytes = 1.28 GB/s raw bandwidth
- With 50% compression: 640 MB/s savings
- At $0.02/GB egress cost: ~$450/hour savings at scale

Existing compressors cannot achieve this because:
- **zlib**: Designed for files, not packets. 200–400 MB/s throughput.
- **LZ4**: Good speed but poor ratio on structured data without dictionary.
- **Zstd**: Excellent ratio with dictionary but 1–5µs per small packet latency.
- **Static Huffman**: Fast but no inter-packet correlation exploitation.

### 2.2 The Gap

There is no open-source, production-quality C library that:
1. Achieves > 2 GB/s compression throughput on structured packets
2. Trains a dictionary from representative traffic
3. Exploits inter-packet correlation (delta encoding)
4. Works correctly for both stateful (ordered stream) and stateless (independent datagram) scenarios
5. Provides comprehensive benchmarks against competing algorithms
6. **Matches OodleNetwork's compression ratio and throughput** — the only existing library that does this is proprietary and requires a commercial license

The absence of an open-source Oodle-grade alternative is the core problem netc solves.

---

## 3. Goals

### 3.1 Primary Goals (P0)

> **The project is NOT considered complete until G-OOD is achieved.**
> All other goals are milestones on the path to G-OOD.

| ID | Goal |
|----|------|
| **G-OOD** | **Benchmark parity with OodleNetwork 2.9.13: compression ratio ≤ Oodle ratio AND throughput ≥ Oodle throughput on WL-001 through WL-005. This is the definition of functional completion.** |
| G-01 | Compress structured binary packets at ≥ 2 GB/s (single core) — intermediate milestone |
| G-02 | Decompress at ≥ 4 GB/s (single core) — intermediate milestone |
| G-03 | Per-packet compression latency p99 ≤ 1 µs |
| G-04 | Achieve ≤ 0.55× compression ratio on game state packets with trained dictionary |
| G-05 | Zero dynamic allocation in hot path |
| G-06 | Correct round-trip for all valid inputs (100% fidelity) |
| G-07 | NEVER expand payload size (automatic passthrough for incompressible data) |
| G-08 | Comparative benchmark harness vs. zlib, LZ4, Zstd, Huffman, **and OodleNetwork** |
| G-09 | Clean C11 API, no dependencies beyond libc |
| G-10 | 95%+ test coverage |

### 3.2 Secondary Goals (P1)

| ID | Goal |
|----|------|
| G-11 | AVX2/NEON SIMD acceleration for bulk operations |
| G-12 | Profile-guided optimization (PGO) support |
| G-13 | TCP stateful stream compression with ring buffer history |
| G-14 | UDP stateless per-packet compression |
| G-15 | Thread-safe context model (one context per thread) |
| G-16 | Dictionary training from packet capture files (pcap) |
| G-17 | **Native C++ SDK** with RAII wrappers for Unreal Engine 5 integration |
| G-18 | **Native C# SDK** with Span-based, GC-pressure-free API for Unity |
| G-19 | **GDExtension SDK** for Godot 4 with MultiplayerPeer integration (C++) |

### 3.3 Non-Goals (Out of Scope)

- Transport-layer sockets (TCP/UDP implementation)
- Encryption or message authentication
- Packet fragmentation/reassembly
- JavaScript/Python bindings (future work)
- Lossy compression

---

## 4. Target Users

| User | Use Case | Key Requirement |
|------|----------|-----------------|
| Game server developers | Reduce bandwidth for entity state sync | < 1µs latency, ≥ 5 Mpps |
| Unreal Engine 5 developers | Reduce replication bandwidth, 100+ player servers | C++ SDK, UE5 plugin, zero alloc |
| Unity developers | Compress Mirror/FishNet/NGO transport packets | C# SDK, zero GC pressure |
| Godot 4 developers | Compress ENet/WebRTC multiplayer packets | GDExtension SDK, MultiplayerPeer wrapper |
| Financial trading systems | Compress market data tick streams | Deterministic latency, high ratio |
| IoT/telemetry platforms | Aggregate sensor packet streams | Low memory footprint, good ratio |
| Network researchers | Benchmark compression algorithms | Reproducible benchmark harness |
| C/C++ library authors | Embed as compression backend | Clean API, no dependencies |

---

## 5. Architecture

### 5.1 Module Breakdown

```
netc/
├── include/
│   └── netc.h              # Public API header
├── src/
│   ├── core/
│   │   ├── netc_ctx.c      # Context management
│   │   ├── netc_dict.c     # Dictionary training and serialization
│   │   ├── netc_compress.c # Compression entry point
│   │   └── netc_decompress.c # Decompression entry point
│   ├── algo/
│   │   ├── netc_ans.c      # ANS (Asymmetric Numeral Systems) codec
│   │   ├── netc_delta.c    # Delta prediction encoder/decoder
│   │   ├── netc_huffman.c  # Fallback Huffman codec
│   │   └── netc_passthru.c # Passthrough for incompressible data
│   ├── simd/
│   │   ├── netc_simd_sse42.c  # SSE4.2 acceleration
│   │   ├── netc_simd_avx2.c   # AVX2 acceleration
│   │   ├── netc_simd_neon.c   # ARM NEON acceleration
│   │   └── netc_simd_generic.c # Generic fallback
│   └── util/
│       ├── netc_crc32.c    # CRC32 for dictionary validation
│       ├── netc_bitstream.c # Bit-level I/O
│       └── netc_stats.c    # Statistics collection
├── bench/
│   ├── bench_main.c        # Benchmark harness entry point
│   ├── bench_corpus.c      # Workload generators
│   ├── bench_timer.c       # High-resolution timing
│   ├── bench_report.c      # CSV/JSON/table output
│   └── bench_compressors/  # Adapters for zlib, LZ4, Zstd, etc.
├── tests/
│   ├── test_compress.c     # Compression correctness tests
│   ├── test_decompress.c   # Decompression correctness tests
│   ├── test_dict.c         # Dictionary training tests
│   ├── test_api.c          # API contract tests
│   ├── test_fuzz.c         # Fuzz/property-based tests
│   └── test_bench.c        # Benchmark gate tests
├── docs/
│   ├── PRD.md              # This document
│   ├── rfc/                # RFC specifications
│   │   ├── RFC-001-netc-compression-protocol.md
│   │   └── RFC-002-benchmark-performance-requirements.md
│   └── design/             # Design documents
│       └── algorithm-decisions.md
├── CMakeLists.txt          # CMake build system
├── Makefile                # Simple make wrapper
└── README.md               # Project overview
```

### 5.2 Compression Pipeline

```
Packet Input
    │
    ├─ size < 8 bytes? ──► Passthrough
    │
    ├─ Entropy check (fast) ──► High entropy? ──► Passthrough
    │
    ▼
Delta Prediction (subtract predicted bytes)
    │
    ▼
ANS Encoding (rANS for large, tANS for small packets)
    │
    ├─ compressed_size >= original_size? ──► Passthrough
    │
    ▼
Bitstream packing + header
    │
    ▼
Compressed output
```

---

## 6. Implementation Phases

### Phase 1: Foundation (Tasks: P1-*)

Deliverables:
- Repository structure, CMake build system
- `netc.h` public API header (complete)
- Core context management (`netc_ctx.c`)
- Passthrough compression (no-op, baseline)
- Basic test framework
- CI pipeline

### Phase 2: Core Algorithm (Tasks: P2-*)

Deliverables:
- ANS codec (`netc_ans.c`)
- Dictionary training (`netc_dict.c`)
- Compression/decompression with dictionary
- CRC32 validation
- Bitstream I/O

### Phase 3: Delta Prediction (Tasks: P3-*)

Deliverables:
- Inter-packet delta encoder/decoder
- Ring buffer history (TCP mode)
- Stateless delta (UDP mode with sequence numbers)
- Bigram context model

### Phase 4: SIMD Acceleration (Tasks: P4-*)

Deliverables:
- CPUID/AT_HWCAP detection
- SSE4.2 bulk operations
- AVX2 acceleration
- ARM NEON support
- Generic fallback path

### Phase 5: Benchmark Harness (Tasks: P5-*)

Deliverables:
- All 8 workload generators (WL-001 through WL-008)
- Adapters for zlib, LZ4, Zstd, Huffman, Snappy
- RDTSC timing infrastructure
- CSV/JSON/table reporting
- CI gate checker (`--ci-check`)

### Phase 6: Hardening & Release (Tasks: P6-*)

Deliverables:
- Fuzz testing (AFL/libFuzzer)
- Security validation (decompressor safety)
- PGO build support
- 95%+ test coverage
- Documentation complete
- Performance meets all RFC-002 gates

---

## 7. Success Metrics

The project is **functionally complete** when all of the following are true simultaneously:

| Metric | Required for completion | Notes |
|--------|------------------------|-------|
| **Compression ratio vs. OodleNetwork (WL-001..005)** | **≤ Oodle ratio** | **Primary completion criterion** |
| **Compress throughput vs. OodleNetwork (WL-001..005)** | **≥ Oodle throughput** | **Primary completion criterion** |
| **Decompress throughput vs. OodleNetwork (WL-001..005)** | **≥ Oodle throughput** | **Primary completion criterion** |
| Compression throughput (absolute) | ≥ 2 GB/s | Floor; Oodle target will be higher |
| Decompression throughput (absolute) | ≥ 4 GB/s | Floor; Oodle target will be higher |
| vs. LZ4 throughput | > LZ4 | Intermediate milestone |
| vs. zlib throughput | > zlib | Intermediate milestone |
| vs. Zstd throughput | > Zstd | Intermediate milestone |
| Compression ratio (game packets) | ≤ 0.55 | Floor; Oodle ratio will be lower |
| Latency p99 (128B packet) | ≤ 1 µs | Hard requirement |
| Test coverage | ≥ 95% | Hard requirement |
| CI benchmark gates passing | 11/11 (open-source) | Hard requirement |
| Oodle benchmark gates passing | 3/3 (OODLE-01,03,05) | **Hard requirement for completion** |

**If Oodle parity cannot be achieved with the initially planned algorithm, the algorithm strategy MUST be revised.** The implementation plan is a means to the target — not the target itself.

---

## 8. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **tANS alone cannot match Oodle ratio** | Medium | **Critical** | **Escalation path: evaluate rANS, higher-order context models, or hybrid Huffman+ANS. Algorithm strategy changes, Oodle target does not.** |
| **SIMD insufficient to close throughput gap vs. Oodle** | Medium | **Critical** | **Escalation path: investigate Oodle's encoding strategy via reverse-benchmark (what htbits setting, what training size achieves its ratio?), then match approach.** |
| **Dictionary training quality below Oodle** | High | **Critical** | **Oodle uses SelectDictionary optimization (trial-based). netc must implement equivalent corpus selection, not just frequency tables.** |
| ANS codec implementation complexity | Medium | High | Reference implementations exist (Zstd/FSE source) |
| SIMD gains smaller than expected | Low | Medium | Generic path still viable as fallback, but Oodle parity likely requires SIMD |
| Benchmark comparison unfair | Low | High | Document exact compiler flags, htbits, corpus sizes for both netc and Oodle |
| Passthrough edge cases missed | Low | High | Extensive fuzz testing in Phase 6 |

### 8.1 Algorithm Escalation Protocol

If at any benchmark checkpoint netc fails to meet the Oodle parity targets (G-OOD), the following escalation process applies — in order:

1. **Profile the gap**: Run `--with-oodle` benchmark, identify whether the gap is ratio, compress throughput, or decompress throughput.
2. **Ratio gap**: Increase context model fidelity (more buckets, higher-order bigram, corpus-based bucket tuning). Evaluate rANS if tANS table resolution is the bottleneck.
3. **Compress throughput gap**: Profile hot path with perf/VTune. Increase SIMD coverage. Evaluate encode parallelism.
4. **Decompress throughput gap**: Verify tANS table fits in L1. Reduce branch count in decode loop. Evaluate prefetch hints.
5. **All else fails**: Study OodleNetwork1_SelectDictionaryFromPackets_Trials approach — Oodle's dictionary quality advantage is likely the root cause of ratio gaps.

No release ships until G-OOD is met.

---

## 9. Dependencies

| Dependency | Purpose | Required? |
|------------|---------|-----------|
| libc | Standard C library | Yes |
| zlib 1.3.x | Benchmark comparison only | Benchmark only |
| LZ4 1.9.x | Benchmark comparison only | Benchmark only |
| Zstd 1.5.x | Benchmark comparison only | Benchmark only |
| CMake 3.20+ | Build system | Yes |
| Unity (C test framework) | Unit testing | Yes (test builds) |

---

*End of PRD*
