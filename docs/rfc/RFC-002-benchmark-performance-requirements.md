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

This document defines the performance targets, benchmark methodology, and comparative test suite for the netc library. It establishes pass/fail criteria against reference compressors (zlib, LZ4, Zstd, static Huffman) across realistic network packet workloads.

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

### 1.3 Comparison Targets (MUST beat)

netc MUST outperform all reference compressors on at least ONE of:
- Throughput (MB/s), OR
- Latency (ns/packet), OR
- Compression ratio

For the structured game packet workload, netc MUST beat ALL reference compressors on throughput (MB/s).

---

## 2. Benchmark Methodology

### 2.1 Measurement Environment

```
Hardware:
  CPU:    x86_64, modern (post-2018), no thermal throttling
  RAM:    ≥ 16 GB DDR4, no NUMA interference
  OS:     Linux (primary), Windows (secondary)
  Cores:  Single-core measurement (no NUMA, no migration)

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
Compressor    | Workload       | p50(ns) | p99(ns) | p999(ns) | MB/s   | Ratio
──────────────┼────────────────┼─────────┼─────────┼──────────┼────────┼──────
netc          | game-64b       |   180   |   420   |    900   | 4800   | 0.48
lz4           | game-64b       |   310   |   680   |   1200   | 2100   | 0.71
zlib-1        | game-64b       |  2800   |  5200   |   9000   |  280   | 0.52
zstd-3        | game-64b       |   950   |  1800   |   3200   |  890   | 0.44
huffman-static| game-64b       |   220   |   450   |   800    | 3200   | 0.58
```

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

### 4.2 Fair Comparison Rules

1. All compressors tested with pre-warmed caches
2. Dictionary-capable compressors (Zstd, netc) trained on same corpus
3. Non-dictionary compressors measured without per-packet dictionary overhead
4. Output size comparison uses final compressed bytes only (no framing overhead)
5. Both compression AND decompression measured separately

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

| Gate | Criterion |
|------|-----------|
| COMP-01 | netc throughput > LZ4 throughput | Required |
| COMP-02 | netc throughput > zlib throughput | Required |
| COMP-03 | netc throughput > Zstd throughput | Required |
| COMP-04 | netc ratio ≤ LZ4 ratio OR netc throughput ≥ 1.5× LZ4 throughput | Required |

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
│   └── bench_huffman.c # Reference Huffman adapter
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

# Run CI gate check
./bench --ci-check  # Returns exit code 0 = pass, 1 = fail
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

*End of RFC-002*
