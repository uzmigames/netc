# netc — Product Requirements Document (PRD)

```
Version:  0.1.0
Date:     2026-02-26
Status:   APPROVED FOR DEVELOPMENT
```

---

## 1. Executive Summary

**netc** is an open-source C library for compressing low-entropy binary network packets at wire speed. It targets high-frequency network scenarios (game servers, financial systems, telemetry) where millions of small packets per second must be compressed with sub-microsecond latency. netc is inspired by RAD Game Tools' Oodle Network and improves upon general-purpose compressors (zlib, LZ4, Zstd) for structured protocol data.

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
4. Works correctly for both TCP (stateful) and UDP (stateless) scenarios
5. Provides comprehensive benchmarks against competing algorithms

---

## 3. Goals

### 3.1 Primary Goals (P0)

| ID | Goal |
|----|------|
| G-01 | Compress structured binary packets at ≥ 2 GB/s (single core) |
| G-02 | Decompress at ≥ 4 GB/s (single core) |
| G-03 | Per-packet compression latency p99 ≤ 1 µs |
| G-04 | Achieve ≤ 0.55× compression ratio on game state packets with trained dictionary |
| G-05 | Zero dynamic allocation in hot path |
| G-06 | Correct round-trip for all valid inputs (100% fidelity) |
| G-07 | NEVER expand payload size (automatic passthrough for incompressible data) |
| G-08 | Comparative benchmark harness vs. zlib, LZ4, Zstd, Huffman |
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

| Metric | Baseline (today) | Target |
|--------|-----------------|--------|
| Compression throughput | N/A | ≥ 2 GB/s |
| vs. LZ4 throughput | N/A | > LZ4 |
| vs. zlib throughput | N/A | > zlib (×5 minimum) |
| Compression ratio (game packets) | N/A | ≤ 0.55 |
| Test coverage | 0% | ≥ 95% |
| CI benchmark gates passing | 0/11 | 11/11 |

---

## 8. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| ANS codec complexity higher than expected | Medium | High | Reference implementations exist (Zstd source) |
| SIMD gains smaller than expected | Low | Medium | Generic path still meets targets without SIMD |
| Dictionary training insufficient ratio | Medium | High | Implement bigram context model in Phase 3 |
| Benchmark comparison unfair | Low | High | Document exact compiler flags and configurations |
| Passthrough edge cases missed | Low | High | Extensive fuzz testing in Phase 6 |

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
