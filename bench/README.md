# bench — netc Benchmark Harness

Standalone benchmark binary for measuring netc compression performance across all RFC-002 workloads.

Supports optional comparison against **zlib**, **LZ4**, **Zstd**, and (with the UE5 SDK) **OodleNetwork**.

---

## Build

```bash
# Standard build (auto-detects zlib/LZ4/Zstd via CMake)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_BENCH=ON
cmake --build build --target bench -j$(nproc)

# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64 -DNETC_BUILD_BENCH=ON
cmake --build build --config Release --target bench
```

If zlib, LZ4, or Zstd are installed, they are automatically detected and the corresponding adapters are compiled in. On Ubuntu:

```bash
sudo apt-get install zlib1g-dev liblz4-dev libzstd-dev
```

On macOS (Homebrew):

```bash
brew install lz4 zstd
```

---

## Quick Start

```bash
# Run all workloads with netc (default)
./build/bench/bench

# Run a specific workload
./build/bench/bench --workload=WL-001

# Run all workloads, compare netc vs LZ4 and Zstd
./build/bench/bench --compressor=netc --compressor=lz4 --compressor=zstd-1 --format=table

# Run all compressors for comparison
./build/bench/bench --compressor=all --format=table

# Export results to CSV
./build/bench/bench --compressor=all --format=csv --output=results.csv

# Export results to JSON (for dashboards / CI artifacts)
./build/bench/bench --compressor=all --format=json --output=results.json
```

---

## CI Gate Check

```bash
# Run CI gates — exits 0 if all pass, 1 if any fail
./build/bench/bench --ci-check --count=100000 --seed=42

# CI gates with comparison compressors (COMP-* gates)
./build/bench/bench --ci-check --compressor=all --count=100000 --seed=42
```

Gates enforced (RFC-002 §6):

| Gate | Criterion |
|------|-----------|
| PERF-01 | Compress throughput ≥ 2 GB/s (WL-001) |
| PERF-02 | Decompress throughput ≥ 4 GB/s (WL-001) |
| PERF-03 | Compress p99 latency ≤ 1 µs (WL-002, 128B) |
| PERF-04 | Decompress p99 latency ≤ 500 ns (WL-002, 128B) |
| PERF-05 | Compress ≥ 5 Mpps (WL-001, 64B) |
| PERF-06 | Decompress ≥ 10 Mpps (WL-001, 64B) |
| RATIO-01 | Ratio ≤ 0.55 (WL-001, trained dict) |
| RATIO-02 | Ratio ≤ 1.01 (WL-006, random passthrough) |
| SAFETY-01 | 100,000 packets roundtrip correctly |
| MEM-01 | Context ≤ 512 KB |
| COMP-\* | netc compress_mbs > each competitor (WL-001) |

---

## CLI Reference

```
Options:
  --workload=WL-NNN     Run specific workload (may repeat; default: all)
                          WL-001  Game state 64B
                          WL-002  Game entity 128B
                          WL-003  Entity snapshot 256B
                          WL-004  Financial tick 32B
                          WL-005  Telemetry 512B
                          WL-006  Random data 128B
                          WL-007  Repetitive data 128B
                          WL-008  Mixed traffic (var)

  --compressor=NAME     Select compressor(s) (may repeat; default: netc)
                          netc          netc stateful+delta+dict
                          zlib-1        zlib level 1 (fastest)
                          zlib-6        zlib level 6 (default)
                          lz4           LZ4 fast (LZ4_compress_default)
                          lz4-hc        LZ4 HC (max level)
                          zstd-1        Zstd level 1
                          zstd-3        Zstd level 3
                          zstd-1-dict   Zstd level 1 + trained dictionary
                          all           All of the above

  --count=N             Measurement iterations [default: 100000]
  --warmup=N            Warmup iterations [default: 1000]
  --seed=N              PRNG seed (reproducible corpus) [default: 42]
  --train=N             Training corpus size for dict [default: 50000]
  --format=FMT          Output: table|csv|json [default: table]
  --output=FILE         Write output to FILE [default: stdout]
  --ci-check            Run CI gates, exit 0=pass 1=fail
  --no-dict             Skip dictionary training (netc only)
  --no-delta            Disable delta prediction (netc only)
  --simd=LEVEL          Force SIMD: auto|generic|sse42|avx2 [default: auto]
```

---

## OodleNetwork Comparison

To compare against OodleNetwork (requires a UE5 license):

```bash
# Build with Oodle SDK
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DNETC_BUILD_BENCH=ON \
  -DNETC_BENCH_WITH_OODLE=ON \
  -DUE5_OODLE_SDK=/path/to/UE5/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13

cmake --build build --target bench -j$(nproc)

# Run comparison (UDP stateless mode)
./build/bench/bench \
  --compressor=netc \
  --compressor=oodle-udp \
  --workload=WL-001 \
  --count=100000 \
  --format=table
```

**Oodle SDK path** (typical UE5 source install):
```
Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13/
├── include/
│   └── oodle2net.h
└── lib/
    └── Win64/
        └── oo2net_win64.lib
```

Alternatively, set the environment variable:
```bash
export UE5_OODLE_SDK=/path/to/OodleNetwork/Sdks/2.9.13
cmake -B build -DNETC_BENCH_WITH_OODLE=ON ...
```

---

## Output Formats

### Table (default)

```
compressor   workload  pkt(B)  ratio  comp(GB/s)  decomp(GB/s)  p99 comp(ns)  p99 decomp(ns)
netc         WL-001       64   0.47       3.1          7.2            380           190
lz4-fast     WL-001       64   0.71       2.0          5.1            610           300
zstd-1       WL-001       64   0.52       0.9          2.8           1100           540
```

### CSV

Schema per RFC-002 §7.3:

```
compressor,cfg,workload,pkt_bytes,count,seed,ratio,orig_bytes,comp_bytes,
comp_mean_ns,comp_p50_ns,comp_p99_ns,comp_mbs,comp_mpps,
decomp_mean_ns,decomp_p50_ns,decomp_p99_ns,decomp_mbs,decomp_mpps
```

### JSON

Schema per RFC-002 §7.4 — includes `version`, `cpu`, `timestamp`, and per-result entries.

---

## Workload Definitions

| ID | Size | Content |
|----|-----:|---------|
| WL-001 | 64B | Game state: player_id, seq, tick, pos[3], vel[3], flags |
| WL-002 | 128B | Extended entity: pos, vel, rot, anim, health, inventory |
| WL-003 | 256B | Full snapshot: entity state + events + metadata |
| WL-004 | 32B | Financial tick: price (random walk), volume (log-normal), symbol |
| WL-005 | 512B | Telemetry: 32 sensor readings + 32 counters + 64 enum values |
| WL-006 | 128B | High-entropy random (tests passthrough path) |
| WL-007 | 128B | Highly repetitive: zeros, ones, alternating 0xAA/0x55 |
| WL-008 | var | Mixed: 60% WL-001 + 20% WL-002 + 10% WL-005 + 10% WL-006 |

All workloads use `splitmix64` PRNG seeded with `--seed` for reproducibility.

---

## Notes

- The bench binary does **not** run as part of `ctest` — it is a standalone tool.
- The GitHub Actions `benchmark` job runs with `--count=100000` and uploads `bench_results.json` as an artifact (30-day retention).
- COMP-* gates (`--ci-check --compressor=all`) are advisory on CI because bare-metal performance varies; they are **required** to pass before a release tag.
- Dictionary training uses the same `--seed` as the benchmark corpus for a fair comparison.
