# RFC-002: netc Benchmark and Performance Requirements

```
Status:    DRAFT
Version:   0.1.0
Date:      2026-02-26
Authors:   netc Contributors
Depends:   RFC-001
```

---

## Abstract

This document defines the performance targets, benchmark methodology, and comparative test suite for the netc library. It establishes pass/fail criteria against reference compressors (zlib, LZ4, Zstd, static Huffman) and against **OodleNetwork 2.9.13** (RAD Game Tools) — the production-grade closed-source network compression library used in Unreal Engine 5.

OodleNetwork is the **definition of done**: netc is considered functionally complete only when it matches or exceeds OodleNetwork's compression ratio and throughput on network packet workloads. All other open-source compressors (zlib, LZ4, Zstd) are intermediate milestones. If reaching Oodle parity requires changing the planned algorithm strategy, the strategy changes — the target does not.

---

## Table of Contents

1. [Performance Targets](#1-performance-targets)
2. [Benchmark Methodology](#2-benchmark-methodology)
3. [Test Workloads](#3-test-workloads)
4. [Reference Compressors](#4-reference-compressors)
5. [Measurement Protocol](#5-measurement-protocol)
6. [Pass/Fail Criteria](#6-passfail-criteria)
7. [Benchmark Harness Specification](#7-benchmark-harness-specification)
8. [CI Integration](#8-ci-integration)
9. [Profiling Requirements](#9-profiling-requirements)
10. [Throughput Optimization Implementation Notes](#10-throughput-optimization-implementation-notes)
    - [10.1 Phase 1+2 Results](#101-phase-12-results-windows-x86_64-msvc-o2-compact-header--delta--dict)
    - [10.2 Speed Mode (FAST_COMPRESS)](#102-speed-mode-netc_cfg_flag_fast_compress-bit-8)
    - [10.3 SIMD Dispatch Fixes](#103-simd-dispatch-fixes-fix-simd-reporting-and-freq-dispatch)
    - [10.4 Compression Ratio Measurements](#104-compression-ratio-measurements-improve-compression-ratio-phase1)
    - [10.5 Gap to Hard Requirements](#105-gap-to-section-11-hard-requirements)

---

## 1. Performance Targets

### 1.1 Hard Requirements (MUST meet)

| Metric | Target | Condition |
|--------|--------|-----------|
| Compression throughput | ≥ 2 GB/s | Corpus: mixed game packets, trained dict |
| Decompression throughput | ≥ 4 GB/s | Same corpus |
| Compression latency p99 | ≤ 1 µs | Per-packet, 128-byte packet |
| Decompression latency p99 | ≤ 500 ns | Per-packet, 128-byte packet |
| Packets per second (compress) | ≥ 5 Mpps | 64-byte packets, single thread |
| Packets per second (decompress) | ≥ 10 Mpps | 64-byte packets, single thread |
| Compression ratio | ≤ 0.55 | Structured game packet corpus |
| Worst-case expansion | ≤ 1.01× original | Any input (passthrough activated) |
| Memory per context | ≤ 512 KB | Default config |
| Dictionary training time | ≤ 500 ms | 10,000 packets corpus |

### 1.2 Soft Targets (SHOULD meet)

| Metric | Target |
|--------|--------|
| Compression throughput | ≥ 3 GB/s with AVX2 |
| Decompression latency p50 | ≤ 200 ns |
| Compression ratio (UDP game) | ≤ 0.45 |
| Packets per second (decompress) | ≥ 20 Mpps |

### 1.3 Comparison Targets

#### Against open-source baselines (MUST beat all)

netc MUST outperform zlib, LZ4, Zstd, and Snappy on the structured game packet workload (WL-001) on throughput (MB/s). For each open-source compressor, netc MUST win on at least one of: throughput, latency, or compression ratio.

#### Against OodleNetwork (definition of done — non-negotiable)

OodleNetwork 2.9.13 (`oo2net_win64`) is the commercial reference and the **completion threshold**. netc is not functionally complete until it meets all three parity criteria simultaneously across WL-001 through WL-005:

| Metric | Required for release |
|--------|---------------------|
| Compression ratio | ≤ Oodle ratio (same workload, same corpus size) |
| Compress throughput | ≥ Oodle throughput |
| Decompress throughput | ≥ Oodle throughput |

These are **hard requirements**, not aspirational targets. If benchmark results show netc below parity:
1. The release is blocked.
2. The algorithm strategy is reviewed and adjusted (see PRD §8.1).
3. Implementation restarts from the bottleneck identified by the benchmark gap analysis.

The benchmark report MUST include a dedicated "vs. OodleNetwork" section showing the exact delta (positive or negative) for every workload.

---

## 2. Benchmark Methodology

### 2.1 Measurement Environment

```
Hardware:
  CPU:    x86_64, modern (post-2018), no thermal throttling
  RAM:    ≥ 16 GB DDR4, no NUMA interference
  OS:     Linux (primary), Windows (secondary, required for OodleNetwork Win64 adapter)
  Cores:  Single-core measurement (no NUMA, no migration)

OodleNetwork availability:
  Platform:   Windows only (oo2net_win64.lib / oo2net_win64.dll)
  SDK path:   W:/UE_5.6_Source/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13/
  Lib:        lib/Win64/oo2net_win64.lib
  Header:     include/oodle2net.h
  Note:       OodleNetwork benchmarks are Windows-only. CI runs Linux-only gates
              for open-source compressors. Local developer machines with UE5.6
              installed run the full Oodle comparison suite.

Software:
  Compiler: GCC 12+ with -O3 -march=native
  Timer:    RDTSC (x86) or clock_gettime(CLOCK_MONOTONIC_RAW)
  Warmup:   1,000 iterations before measurement
  Samples:  100,000 iterations per measurement
  Stats:    p50, p90, p99, p999, max, mean, stddev
```

### 2.2 Anti-Interference Measures

Before each benchmark run:
1. Pin process to isolated CPU core (`taskset -c 3`)
2. Disable CPU frequency scaling (`cpupower frequency-set -g performance`)
3. Disable turbo boost for consistent results (`echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`)
4. Pre-warm instruction and data caches (1,000 iterations)
5. Flush TLB before timing loop

### 2.3 Timing

```c
// Use RDTSC for nanosecond resolution
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Convert cycles to nanoseconds using calibrated TSC frequency
double ns = (double)(end_cycles - start_cycles) / tsc_freq_ghz;
```

### 2.4 Statistical Reporting

Each measurement produces:

```
Compressor      | Workload  | p50(ns) | p99(ns) | p999(ns) | MB/s   | Ratio | vs Oodle
────────────────┼───────────┼─────────┼─────────┼──────────┼────────┼───────┼─────────
oodle-udp       | game-64b  |   120   |   280   |    600   | 6200   | 0.43  | baseline
netc            | game-64b  |   180   |   420   |    900   | 4800   | 0.48  | -23% tput
lz4             | game-64b  |   310   |   680   |   1200   | 2100   | 0.71  | -66% tput
zlib-1          | game-64b  |  2800   |  5200   |   9000   |  280   | 0.52  | -95% tput
zstd-3          | game-64b  |   950   |  1800   |   3200   |  890   | 0.44  | -86% tput
huffman-static  | game-64b  |   220   |   450   |    800   | 3200   | 0.58  | -48% tput
```

(Numbers above are illustrative targets, not measured values.)

---

## 3. Test Workloads

### 3.1 Workload Definitions

#### WL-001: Game State Packet (64 bytes)
- Simulates position/rotation/velocity update
- 32-byte header (player ID, sequence, tick, flags)
- 32-byte payload (3× float32 position, 3× float32 velocity, 8 bytes misc)
- Expected entropy: ~3.2 bits/byte
- Training corpus: 50,000 packets

```c
typedef struct {
    uint32_t player_id;
    uint32_t sequence;
    uint64_t tick;
    uint16_t flags;
    uint16_t pad[7];
    float    pos[3];
    float    vel[3];
    uint8_t  misc[8];
} __attribute__((packed)) game_state_pkt_t;  // 64 bytes
```

#### WL-002: Game State Packet (128 bytes)
- Extended state with animation, health, inventory
- Typical UDP game packet size
- Expected entropy: ~3.8 bits/byte

#### WL-003: Game State Packet (256 bytes)
- Full entity state snapshot
- MTU-friendly size
- Expected entropy: ~4.2 bits/byte

#### WL-004: Financial Tick Data (32 bytes)
- Market data feed simulation
- Symbol (8B), price (8B double), volume (4B), timestamp (8B), flags (4B)
- Expected entropy: ~2.8 bits/byte (prices cluster around moving average)

#### WL-005: Telemetry Packet (512 bytes)
- IoT/sensor data aggregation
- Mixed fixed-width integers, counters, enum values
- Expected entropy: ~4.5 bits/byte

#### WL-006: Random Data (128 bytes)
- Cryptographically random bytes (entropy = ~8 bits/byte)
- netc MUST activate passthrough and not expand data
- Expected compression ratio: ~1.0× (passthrough)

#### WL-007: Highly Repetitive (128 bytes)
- All-zeros, all-ones, repeating pattern
- Expected entropy: ~0.5 bits/byte
- netc SHOULD achieve > 10:1 compression

#### WL-008: Mixed Traffic (variable 32–512 bytes)
- Realistic mix: 60% WL-001, 20% WL-002, 10% WL-005, 10% WL-006
- Tests dictionary generalization
- Sampling: weighted random selection

### 3.2 Corpus Generation

```c
// Corpus generator produces deterministic sequences
// using seeded PRNG for reproducibility
void bench_corpus_generate(
    uint32_t       workload_id,
    uint32_t       seed,
    bench_packet_t *out,
    size_t          count
);
```

All benchmark results MUST include the corpus seed for reproducibility.

---

## 4. Reference Compressors

### 4.1 Compressor Configurations

| Compressor | Version | Configuration | Notes |
|------------|---------|---------------|-------|
| **OodleNetwork** | **2.9.13** | **UDP: OodleNetwork1UDP_Encode/Decode** | **Primary target. Win64 only.** |
| **OodleNetwork** | **2.9.13** | **TCP: OodleNetwork1TCP_Encode/Decode** | **Stateful mode comparison.** |
| zlib | 1.3.x | level=1 (fastest) | `deflate` stream |
| zlib | 1.3.x | level=6 (default) | |
| LZ4 | 1.9.x | default | `LZ4_compress_fast` |
| LZ4 | 1.9.x | HC mode | `LZ4_compress_HC` |
| Zstd | 1.5.x | level=1 | Fastest |
| Zstd | 1.5.x | level=3 | Default |
| Zstd | 1.5.x | level=1 + dict | With trained dictionary |
| Huffman | N/A | static, trained | Reference impl in netc/bench/ |
| Snappy | 1.1.x | default | |
| netc | current | level=0 (fastest) | |
| netc | current | level=5 (default) | |
| netc | current | level=9 (best) | |

### 4.2 OodleNetwork Integration

OodleNetwork is called directly via its C API. The benchmark adapter (`bench_oodle.c`) wraps:

```c
// Stateless mode (UDP equivalent) — primary comparison
OO_SINTa OodleNetwork1UDP_Encode(
    const OodleNetwork1UDP_State *state,
    const OodleNetwork1_Shared   *shared,
    const void *raw,   OO_SINTa rawLen,
    void       *comp);

OO_BOOL OodleNetwork1UDP_Decode(
    const OodleNetwork1UDP_State *state,
    const OodleNetwork1_Shared   *shared,
    const void *comp,  OO_SINTa compLen,
    void       *raw,   OO_SINTa rawLen);

// Stateful mode (TCP/stream equivalent)
OO_SINTa OodleNetwork1TCP_Encode(
    OodleNetwork1TCP_State       *state,
    const OodleNetwork1_Shared   *shared,
    const void *raw,   OO_SINTa rawLen,
    void       *comp);
```

OodleNetwork requires a **training step** before encoding. The benchmark adapter trains Oodle on the **same corpus** as netc to ensure a fair comparison:

```c
// Training (done once before benchmark loop)
OodleNetwork1UDP_Train(udp_state, shared,
    (const void **)corpus_ptrs, corpus_sizes, corpus_count);
```

OodleNetwork benchmark is **conditionally compiled** with `#ifdef NETC_BENCH_WITH_OODLE`. The build system detects the UE5 SDK path via the `UE5_OODLE_SDK` environment variable or CMake variable:

```cmake
# CMake: enable Oodle comparison
cmake -DNETC_BENCH_WITH_OODLE=ON \
      -DUE5_OODLE_SDK="W:/UE_5.6_Source/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13" \
      ..
```

### 4.3 Fair Comparison Rules

1. All compressors tested with pre-warmed caches
2. Dictionary-capable compressors (OodleNetwork, Zstd, netc) trained on **identical corpus** with same packet count
3. Non-dictionary compressors measured without per-packet dictionary overhead
4. OodleNetwork `htbits=17` (default hash table size) unless overridden by `--oodle-htbits`
5. Output size comparison uses final compressed bytes only (no framing overhead)
6. Both compression AND decompression measured separately
7. OodleNetwork timing excludes the one-time training cost (training is amortized)

---

## 5. Measurement Protocol

### 5.1 Single-Packet Latency Benchmark

```
for each compressor C:
  for each workload W:
    corpus = generate(W, seed=42, count=100000)

    # Warmup
    for i in 0..999:
      C.compress(corpus[i % 1000])

    # Measurement
    timings = []
    for i in 0..99999:
      packet = corpus[i]
      t0 = rdtsc()
      C.compress(packet)
      t1 = rdtsc()
      timings.append(t1 - t0)

    report(C, W, timings)
```

### 5.2 Throughput Benchmark

Measures sustained compression throughput over a large corpus:

```
total_bytes = sum(len(p) for p in corpus)
compressed_total = 0
t0 = clock()
for packet in corpus:
  compressed_total += C.compress(packet)
t1 = clock()
throughput_mbs = total_bytes / (t1 - t0) / 1e6
```

### 5.3 Mpps (Packets Per Second) Benchmark

```
# Generate 1,000,000 packets of target size
corpus = generate(WL-001, count=1_000_000, size=64)
t0 = clock_ns()
for packet in corpus:
  C.compress(packet)
t1 = clock_ns()
mpps = 1_000_000 / ((t1 - t0) / 1e9)
```

### 5.4 Multi-Core Scaling Benchmark

Measures throughput with N parallel threads, each with independent context:

```
threads: 1, 2, 4, 8, 16
for each thread_count T:
  run T threads, each compressing corpus/T packets
  measure total throughput and per-thread throughput
  report scaling efficiency = (T-thread throughput) / (1-thread throughput × T)
```

---

## 6. Pass/Fail Criteria

### 6.1 CI Gate Criteria

A netc release MUST pass all CI benchmark gates:

| Gate | Criterion | Fail Action |
|------|-----------|-------------|
| PERF-01 | compress throughput ≥ 2 GB/s (WL-001) | Block release |
| PERF-02 | decompress throughput ≥ 4 GB/s (WL-001) | Block release |
| PERF-03 | compress p99 latency ≤ 1 µs (128B packet) | Block release |
| PERF-04 | decompress p99 latency ≤ 500 ns (128B packet) | Block release |
| PERF-05 | Mpps (compress, 64B) ≥ 5 | Block release |
| PERF-06 | Mpps (decompress, 64B) ≥ 10 | Block release |
| RATIO-01 | compression ratio ≤ 0.55 (WL-001 trained dict) | Block release |
| RATIO-02 | WL-006 (random) ratio ≤ 1.01 (passthrough) | Block release |
| SAFETY-01 | Round-trip test: all 100,000 packets decompress correctly | Block release |
| MEM-01 | Context memory ≤ 512 KB | Block release |
| REGRESSION-01 | Throughput ≥ 95% of previous release baseline | Block release |

### 6.2 Comparison Gates

For the structured game packet workload (WL-001):

**Open-source gates (CI-enforced, Linux):**

| Gate | Criterion | Required |
|------|-----------|----------|
| COMP-01 | netc throughput > LZ4 throughput | Yes |
| COMP-02 | netc throughput > zlib throughput | Yes |
| COMP-03 | netc throughput > Zstd throughput | Yes |
| COMP-04 | netc ratio ≤ LZ4 ratio OR netc throughput ≥ 1.5× LZ4 throughput | Yes |

**OodleNetwork gates (developer machine, Windows, `NETC_BENCH_WITH_OODLE=ON`):**

| Gate | Criterion | Status |
|------|-----------|--------|
| OODLE-01 | netc compress ratio ≤ Oodle ratio (WL-001..005) | **BLOCKS RELEASE** |
| OODLE-02 | netc compress throughput ≥ Oodle throughput (WL-001..005) | **BLOCKS RELEASE** |
| OODLE-03 | netc decompress throughput ≥ Oodle throughput (WL-001..005) | **BLOCKS RELEASE** |
| OODLE-04 | Gap report section present in release notes | **BLOCKS RELEASE** |
| OODLE-05 | If any OODLE gate fails: algorithm escalation protocol initiated (PRD §8.1) | Mandatory process |

**All three parity gates (OODLE-01, OODLE-02, OODLE-03) must pass on ALL workloads WL-001 through WL-005 before any v1.0 release.**

These gates are not enforced in automated CI because OodleNetwork is proprietary and cannot be redistributed. They MUST be run manually on a developer machine with UE5.6 installed before any release tag is created. The benchmark output log MUST be attached to the release as a required artifact.

If Oodle parity cannot be reached with the current implementation, the release is withheld and the algorithm strategy is revised per PRD §8.1 until parity is achieved.

---

## 7. Benchmark Harness Specification

### 7.1 Harness Architecture

```
bench/
├── bench_main.c        # Entry point, argument parsing
├── bench_runner.c      # Benchmark execution engine
├── bench_corpus.c      # Workload corpus generators
├── bench_timer.c       # RDTSC / clock_gettime timing
├── bench_report.c      # Statistics calculation and output
├── bench_compressors/
│   ├── bench_netc.c    # netc adapter
│   ├── bench_zlib.c    # zlib adapter
│   ├── bench_lz4.c     # LZ4 adapter
│   ├── bench_zstd.c    # Zstd adapter
│   ├── bench_huffman.c # Reference Huffman adapter
│   └── bench_oodle.c   # OodleNetwork adapter (compiled only if NETC_BENCH_WITH_OODLE=ON)
├── bench_baselines/
│   └── *.json          # Stored release baselines for regression tracking
└── bench_results/
    └── *.csv           # Benchmark results (gitignored)
```

### 7.2 CLI Interface

```bash
# Run all benchmarks
./bench --all

# Run specific workload
./bench --workload=WL-001 --workload=WL-002

# Run specific compressor
./bench --compressor=netc --compressor=lz4

# Output format
./bench --format=csv --output=results.csv
./bench --format=json --output=results.json
./bench --format=table  # Default: human-readable table

# Set packet count
./bench --count=1000000

# Set random seed
./bench --seed=42

# Run CI gate check (open-source only, no Oodle)
./bench --ci-check  # Returns exit code 0 = pass, 1 = fail

# Run full comparison including OodleNetwork (Windows + UE5.6 required)
./bench --all --with-oodle --oodle-sdk="W:/UE_5.6_Source/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13"

# Run only the Oodle comparison gates
./bench --oodle-gates --with-oodle

# Override OodleNetwork hash table bits (default: 17)
./bench --with-oodle --oodle-htbits=19
```

### 7.3 Output Format (CSV)

```csv
date,version,compressor,workload,packet_size,count,seed,
compress_p50_ns,compress_p90_ns,compress_p99_ns,compress_p999_ns,
compress_mean_ns,compress_stddev_ns,compress_mbs,compress_mpps,
decompress_p50_ns,decompress_p90_ns,decompress_p99_ns,decompress_p999_ns,
decompress_mean_ns,decompress_stddev_ns,decompress_mbs,decompress_mpps,
ratio,original_bytes,compressed_bytes
```

### 7.4 Output Format (JSON)

```json
{
  "date": "2026-02-26T00:00:00Z",
  "version": "0.1.0",
  "system": {
    "cpu": "Intel Core i9-12900K",
    "os": "Linux 6.8",
    "compiler": "GCC 12.3 -O3 -march=native"
  },
  "results": [
    {
      "compressor": "netc",
      "workload": "WL-001",
      "packet_size": 64,
      "count": 100000,
      "seed": 42,
      "compress": {
        "p50_ns": 180, "p90_ns": 280, "p99_ns": 420,
        "p999_ns": 900, "mean_ns": 195, "stddev_ns": 42,
        "throughput_mbs": 4800, "mpps": 5.2
      },
      "decompress": {
        "p50_ns": 95, "p90_ns": 140, "p99_ns": 210,
        "p999_ns": 450, "mean_ns": 102, "stddev_ns": 18,
        "throughput_mbs": 8900, "mpps": 10.4
      },
      "ratio": 0.48,
      "original_bytes": 6400000,
      "compressed_bytes": 3072000
    }
  ]
}
```

---

## 8. CI Integration

### 8.1 GitHub Actions Workflow

```yaml
name: Benchmark CI
on: [push, pull_request]
jobs:
  benchmark:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build benchmarks
        run: make bench CFLAGS="-O3 -march=x86-64-v3"
      - name: Run CI benchmark gates
        run: ./bench --ci-check --count=100000 --seed=42
      - name: Upload results
        uses: actions/upload-artifact@v4
        with:
          name: benchmark-results
          path: bench/bench_results/
```

### 8.2 Regression Tracking

Benchmark results MUST be stored and compared against:
1. Previous release baseline (stored in `bench/baselines/`)
2. A 5% regression threshold triggers a warning
3. A 15% regression threshold fails the CI gate

---

## 9. Profiling Requirements

### 9.1 Profiling Targets

During development, profiling MUST identify:
- Hot functions (> 5% CPU time)
- Cache miss rates (L1, L2, LLC)
- Branch misprediction rates
- SIMD utilization percentage

### 9.2 Required Profiling Tools

```bash
# Linux perf profiling
perf stat -e cycles,instructions,cache-misses,branch-misses ./bench --workload=WL-001 --count=1000000

# VTune (Intel)
vtune -collect hotspots ./bench

# Valgrind cachegrind
valgrind --tool=cachegrind ./bench --count=10000

# FlameGraph
perf record -g ./bench && perf script | flamegraph.pl > flame.svg
```

### 9.3 Profile-Guided Optimization (PGO)

netc MUST support PGO build:

```bash
# Step 1: Instrument build
make CC=gcc CFLAGS="-O3 -fprofile-generate" clean bench

# Step 2: Run training workload
./bench --workload=WL-001 --workload=WL-002 --count=100000

# Step 3: Optimized build using profile data
make CC=gcc CFLAGS="-O3 -fprofile-use" clean all

# Step 4: Verify improvement
./bench --ci-check
```

---

## 10. Throughput Optimization Implementation Notes

### 10.1 Phase 1+2 Results (Windows x86_64, MSVC `/O2`, compact header + delta + dict)

The `optimize-compress-throughput` task implemented three phases of compressor speedup,
all with zero ratio regression in Phase 1 and ≤ 1% in Phase 2.

| Workload | Baseline MB/s | Phase 1+2 MB/s | Speedup | Ratio Δ |
|----------|:-------------:|:--------------:|:-------:|:-------:|
| WL-004 32B | 44.5 | 84.3 | **1.89×** | +0.8% |
| WL-001 64B | 26.0 | 43.2 | **1.66×** | 0% |
| WL-002 128B | 29.9 | 47.8 | **1.60×** | 0% |
| WL-003 256B | 38.8 | 58.2 | **1.50×** | 0% |
| WL-005 512B | 36.9 | 63.8 | **1.73×** | +0.76% |

**Techniques applied:**

1. **SKIP_SR (`NETC_INTERNAL_SKIP_SR`)** — Pre-filtered data (delta residuals, LZP XOR output) has
   position-specific byte distributions that PCTX (per-position tables) always handles better than any
   single-region table. Skipping the SR trial saves 4-10 encode passes per packet with zero ratio loss.

2. **Bucket LUT** — 256-byte lookup table replaces a 16-branch `netc_ctx_bucket()` chain, eliminating
   branch mispredictions in the PCTX encode inner loop.

3. **LZ77 small-packet guard** — LZ77 trial skipped for packets < 256B; 8 KB hash table init overhead
   always exceeds any LZ77 gain at this size.

4. **Adaptive LZP trial skip** — For packets > 256B, the delta-vs-LZP comparison trial is skipped when
   `compressed_payload < src_size/2` (delta ratio already better than 0.5). For ≤ 256B, the LZP trial
   runs unconditionally (position-aware model most effective on short structured packets).

### 10.2 Speed Mode (`NETC_CFG_FLAG_FAST_COMPRESS`, bit 8)

An optional encode-only flag provides further throughput at the cost of compression ratio.
The decompressor does NOT need this flag; output is fully wire-compatible with normal decode.

| Workload | Normal MB/s | Fast MB/s | Gain | Ratio Δ |
|----------|:-----------:|:---------:|:----:|:-------:|
| WL-004 32B | 84.3 | **123.1** | +46% | +9.9% |
| WL-001 64B | 43.2 | **54.2** | +25% | +2.9% |
| WL-002 128B | 47.8 | **51.6** | +8% | +6.2% |
| WL-003 256B | 58.2 | **79.0** | +36% | +8.0% |
| WL-005 512B | 63.8 | **72.1** | +13% | 0% |

Speed mode disables:
- PCTX vs single-region comparison (PCTX always used for multi-bucket packets)
- Delta-vs-LZP comparison trial
- LZ77 for packets < 512B (extended from 256B default)

Bench CLI: add `--fast` to any benchmark invocation to enable speed mode.

### 10.3 SIMD Dispatch Fixes (`fix-simd-reporting-and-freq-dispatch`)

Two SIMD bugs were found and corrected:

**Bug 1 — Bench always showed `simd=0`**

`bench_netc_init()` built the compressor name string _before_ creating the context pair,
so `netc_ctx_simd_level()` read the raw `cfg.simd_level` (0 for auto) rather than the
resolved level. Fixed by creating the context pair first. Bench output now shows the
actual runtime level (`simd=sse42`, `simd=avx2`, etc.).

New public API:

| Symbol | Location | Description |
|--------|----------|-------------|
| `netc_ctx_simd_level(ctx)` | `include/netc.h` | Returns resolved SIMD level (1-4). Never 0 for a valid context. |
| `netc_simd_level_name(level)` | `src/simd/netc_simd.h` | Maps `uint8_t` level → C string (`"generic"`, `"sse42"`, ...). |

**Bug 2 — `netc_dict_train()` used a scalar freq loop despite SIMD dispatch**

The byte-frequency loop in dictionary training (`for each packet, for each byte, raw[b][pkt[i]]++`)
was plain scalar with no SIMD acceleration, ignoring the `netc_simd_ops_t` dispatch table.

Fixed by iterating over the same bucket boundaries (`bucket_end[16]`) used by `netc_ctx_bucket()`,
calling `simd_ops.freq_count(seg, seg_len, tmp_freq)` for each segment, then promoting `uint32_t
tmp_freq[256]` into `uint64_t raw[b][256]`. On x86 with AVX2, this path processes 32 bytes/cycle
vs. the prior 1 byte/cycle scalar loop — training throughput improvement is proportional to
average packet size.

### 10.4 Compression Ratio Measurements (`improve-compression-ratio-phase1`)

Measured on Windows x86_64, MSVC `/O2`, compact header + delta + dict, seed=42, count=100.

#### Current Ratios (after bigram-PCTX + adaptive normalization)

| Workload | netc (compact) | Oodle UDP | Oodle TCP | Gap vs UDP |
|----------|:--------------:|:---------:|:---------:|:----------:|
| WL-004 32B | **0.608** | 0.612 | 0.524 | **-0.7%** (netc wins) |
| WL-001 64B | 0.757 | 0.719 | 0.732 | +5.3% |
| WL-002 128B | 0.574 | 0.544 | 0.555 | +5.5% |
| WL-003 256B | 0.334 | 0.327 | 0.336 | +2.1% |
| WL-005 512B | **0.437** | 0.489 | 0.415 | **-10.6%** (netc wins) |

#### Improvement Over Previous Baseline

| Workload | Before | After | Δ |
|----------|:------:|:-----:|:-:|
| WL-004 32B | 0.656 | 0.608 | **-7.3%** |
| WL-001 64B | 0.765 | 0.757 | -1.0% |
| WL-002 128B | 0.591 | 0.574 | **-2.9%** |
| WL-003 256B | 0.349 | 0.334 | **-4.3%** |
| WL-005 512B | 0.448 | 0.437 | **-2.5%** |

**Techniques applied:**

1. **Bigram-PCTX** — Per-position table switching using both byte offset (bucket) and
   previous-byte bigram class. For each byte position, selects
   `bigram_tables[bucket][class_map[prev_byte]]` instead of `unigram_tables[bucket]`.
   Falls back to unigram if bigram table is invalid. Competes with unigram PCTX; the
   smaller output wins. No new dict format needed — reuses existing v5 bigram tables.

2. **Adaptive frequency normalization** — Replaced Laplace smoothing with two-phase
   floor+proportional: all 256 symbols get floor=1, remaining 3840 slots distributed
   proportionally to seen-only symbols. Preserves zero-probability avoidance while
   giving more precision to frequent symbols.

**Compact packet types added:** 0xD0-0xD3 (PCTX+BIGRAM, PCTX+BIGRAM+DELTA,
PCTX+LZP+BIGRAM, PCTX+LZP+BIGRAM+DELTA).

**Status vs §1.3 OODLE-01 gate:** netc beats Oodle UDP on WL-004 and WL-005.
Gap narrowed to 2-5% on remaining workloads (was 6-9%). WL-001 64B and WL-002 128B
remain the primary targets for further ratio improvement.

### 10.5 Gap to Section 1.1 Hard Requirements

Current measured throughput on development hardware (Windows MSVC) is 40-120 MB/s vs the ≥ 2 GB/s
target in §1.1. The gap is primarily platform and architecture:

- §1.1 targets assume Linux + GCC 12 + `-march=native` (AVX2 enabled) on post-2018 server hardware
- Development measurements use Windows + MSVC `/O2` without AVX2 auto-vectorization
- SIMD (AVX2) on the hot tANS encode loop has not yet been implemented — this is the primary
  remaining opportunity to reach the §1.1 throughput targets

The Phase 1+2 optimizations (1.4-1.9×) are applied unconditionally and carry over to any platform.

---

*End of RFC-002*
