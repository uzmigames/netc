# netc — Network Compression Library

[![Language](https://img.shields.io/badge/language-C11-orange.svg)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Coverage](https://img.shields.io/badge/coverage-95%25+-brightgreen.svg)](tests/)
[![Version](https://img.shields.io/badge/version-0.1.0--dev-blue.svg)](CHANGELOG.md)

> High-performance binary packet compression for low-entropy network payloads. Purpose-built for game netcode, telemetry, and real-time protocols.

---

## Key Features

- **tANS (FSE) entropy coder** — adaptive 12-bit (4096 entries) and 10-bit (1024 entries) tables, branch-free decode, fractional-bit precision
- **LZP prediction pre-filter** — position-aware order-1 context XOR filter, predicted bytes become 0x00
- **Bigram-PCTX context model** — per-position table switching using both byte offset and previous-byte bigram class. Bigram-PCTX competes with unigram PCTX; smallest wins
- **Inter-packet delta prediction** — field-class aware (XOR for flags/floats, subtraction for counters), order-2 linear extrapolation for smooth trends
- **Adaptive cross-packet learning** — frequency tables, LZP hashes, and delta order adapt to the live stream (`NETC_CFG_FLAG_ADAPTIVE`)
- **Compact packet header** — 2B header for packets ≤ 127B, 4B for larger. Opt-in via `NETC_CFG_FLAG_COMPACT_HDR`
- **ANS state compaction** — 2B tANS state in compact mode (vs. 4B legacy)
- **Multi-codec competition** — tANS vs LZ77 vs RLE vs passthrough per packet, smallest wins
- **Dictionary training** — train from packet corpus, freeze for hot-path. v5 format with LZP + 8-class trained bigram tables
- **Stateful & stateless modes** — ring buffer history (TCP) or self-contained per-packet (UDP)
- **SIMD acceleration** — SSE4.2 and AVX2 (x86) with runtime dispatch, generic scalar fallback. `netc_ctx_simd_level(ctx)` reports the resolved level; dict training also uses SIMD freq_count dispatch
- **Zero dynamic allocation in hot path** — pre-allocated arena, deterministic latency
- **Passthrough guarantee** — never expands payload; activates automatically on incompressible data
- **Security hardened** — bounds-checked decompressor, CRC32 dictionary validation, fuzz tested
- **Clean C11 API** — single header `netc.h`, zero dependencies beyond libc

---

## Benchmarks

### Compression Ratio — netc vs Compressors

Measured on Windows x86_64, MSVC `/O2`, 50,000 iterations, 10,000 training packets.
Lower is better (compressed size / original size).

| Compressor | WL-004 (32B) | WL-001 (64B) | WL-002 (128B) | WL-003 (256B) | WL-005 (512B) | Design goal |
|------------|:------------:|:------------:|:-------------:|:-------------:|:-------------:|-------------|
| **netc** (compact header) | **0.658** | **0.758** | **0.571** | **0.331** | **0.437** | Network packets |
| **netc** (legacy 8B header) | 0.906 | 0.890 | 0.638 | 0.373 | — | Network packets |
| OodleNetwork UDP 2.9.13 | 0.658 | 0.765 | 0.573 | 0.345 | 0.476 | Network packets |
| OodleNetwork TCP 2.9.13 | 0.572 | 0.779 | 0.585 | 0.354 | 0.415 | Network packets |
| Zstd (level=1, dict) | 1.321 | 1.034 | 0.746 | 0.430 | 0.574 | General purpose |
| zlib (level=1) | 1.176 | 0.966 | 0.740 | 0.441 | 0.591 | General purpose |
| LZ4 (fast) | 1.056 | 0.875 | 0.744 | 0.478 | 0.703 | Speed-oriented |
| Snappy | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | Speed-oriented |

> **Note:** General-purpose compressors (Zstd, zlib, LZ4, Snappy) all **expand** 32B packets (ratio > 1.0) — their framing overhead exceeds savings. netc and OodleNetwork are purpose-built for small network packets. **netc beats or matches Oodle UDP on all 5 workloads** (32B-512B) with fair train/eval split (seed 42 training, separate eval seed). Previous measurements used overlapping seeds which gave Oodle an unfair window-matching advantage.

### netc Header Overhead Breakdown

| Mode | Header | ANS state | Total overhead | Best for |
|------|:------:|:---------:|:--------------:|----------|
| Compact (≤ 127B packet) | 2B | 2B | **4B** | Small game packets |
| Compact (128-65535B) | 4B | 2B | **6B** | Medium payloads |
| Legacy | 8B | 4B | **12B** | Compatibility |

### Throughput — Measured (Windows x86_64, MSVC `/O2`, compact header + delta + dict)

| Workload | Normal MB/s | Fast MB/s | Ratio (normal) | Ratio (fast) | Baseline MB/s |
|----------|:-----------:|:---------:|:--------------:|:------------:|:-------------:|
| WL-004 32B | 84.3 | **123.1** | 0.631 | 0.694 | 44.5 |
| WL-001 64B | 43.2 | **54.2** | 0.756 | 0.778 | 26.0 |
| WL-002 128B | 47.8 | **51.6** | 0.603 | 0.641 | 29.9 |
| WL-003 256B | 58.2 | **79.0** | 0.343 | 0.370 | 38.8 |
| WL-005 512B | 63.8 | **72.1** | 0.491 | 0.491 | 36.9 |

*Baseline = pre-optimization. Normal = Phase 1+2 (default). Fast = `NETC_CFG_FLAG_FAST_COMPRESS` (speed mode, 0-10% ratio trade-off).
Phase 1+2 achieved 1.4-1.9× speedup with ≤1% ratio regression. Speed mode adds a further 8-62% on top.*

### Throughput Targets for v1.0 (server-grade hardware)

| Metric | Target |
|--------|-------:|
| Compress throughput (AVX2) | ≥ 3 GB/s |
| Decompress throughput (AVX2) | ≥ 7 GB/s |
| Compress p99 latency (128B) | ≤ 1 µs |
| Decompress p99 latency (128B) | ≤ 500 ns |
| Packets/sec compress (64B) | ≥ 5 Mpps |
| Packets/sec decompress (64B) | ≥ 10 Mpps |
| Context memory | ≤ 512 KB |

See [RFC-002](docs/rfc/RFC-002-benchmark-performance-requirements.md) for methodology and workload definitions.

### Gap to OodleNetwork UDP

| Workload | netc (compact) | Oodle UDP | Gap | Notes |
|----------|:--------------:|:---------:|:---:|-------|
| WL-004 32B | **0.658** | 0.658 | **0.0%** | **Tie** — parity on financial tick data |
| WL-001 64B | **0.758** | 0.765 | **-0.8%** | **netc wins** — tANS + delta + bigram-PCTX |
| WL-002 128B | **0.571** | 0.573 | **-0.3%** | **netc wins** — marginal but consistent |
| WL-003 256B | **0.331** | 0.345 | **-3.8%** | **netc wins** — ~3.6B saved per packet |
| WL-005 512B | **0.437** | 0.476 | **-8.2%** | **netc wins** — LZP + bigram-PCTX dominates |

**netc beats or matches OodleNetwork UDP on all workloads.** OODLE-01 gate (ratio parity) is PASSED. Measured with fair train/eval seed split (training on seed=42, evaluation on seed+offset). With `NETC_CFG_FLAG_ADAPTIVE` enabled, netc now performs cross-packet learning (adaptive tANS tables, LZP hash updates, order-2 delta prediction) — closing the gap with Oodle TCP's stateful mode.

---

## Quick Start

### Build (Linux / macOS)

```bash
git clone https://github.com/nicholascuneo/netc.git
cd netc

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

ctest --test-dir build --output-on-failure
```

### Build (Windows / MSVC)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

ctest --test-dir build -C Release --output-on-failure
```

### Run Benchmarks

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --config Release

# Run all workloads
./build-bench/bench --format=table

# Compare specific compressors
./build-bench/bench --workload=WL-001 --compressor=netc --compressor=lz4 --format=csv
```

---

## Usage

### Train a Dictionary

```c
#include "netc.h"

const uint8_t *packets[50000];
size_t         sizes[50000];
// ... fill from network capture ...

netc_dict_t *dict = NULL;
netc_result_t r = netc_dict_train(packets, sizes, 50000, &dict);
assert(r == NETC_OK);

void  *blob;
size_t blob_size;
netc_dict_save(dict, &blob, &blob_size);
// write blob to disk for reuse
```

### Compress (TCP stateful mode)

```c
#include "netc.h"

netc_dict_t *dict = NULL;
netc_dict_load(blob, blob_size, &dict);

netc_cfg_t cfg = {
    .flags = NETC_CFG_FLAG_TCP_MODE | NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR,
};
netc_ctx_t *ctx = netc_ctx_create(dict, &cfg);

uint8_t dst[NETC_MAX_PACKET_SIZE + NETC_HEADER_SIZE];
size_t  dst_size = 0;

netc_result_t r = netc_compress(ctx, src, src_size, dst, sizeof(dst), &dst_size);
if (r == NETC_OK) {
    send(sock, dst, dst_size, 0);
}

// Decompress on the other side
uint8_t recovered[NETC_MAX_PACKET_SIZE];
size_t  recovered_size = 0;
netc_decompress(ctx_remote, dst, dst_size, recovered, sizeof(recovered), &recovered_size);

netc_ctx_destroy(ctx);
netc_dict_free(dict);
```

### Compress (UDP stateless mode)

```c
netc_cfg_t cfg = { .flags = NETC_CFG_FLAG_UDP_MODE | NETC_CFG_FLAG_COMPACT_HDR };
netc_ctx_t *ctx = netc_ctx_create(dict, &cfg);

// Each packet is self-contained
netc_compress_stateless(dict, src, src_size, dst, sizeof(dst), &dst_size);
netc_decompress_stateless(dict, dst, dst_size, recovered, sizeof(recovered), &recovered_size);
```

### Query the Active SIMD Level

```c
#include "netc.h"

netc_cfg_t cfg = { .flags = NETC_CFG_FLAG_TCP_MODE | NETC_CFG_FLAG_DELTA };
netc_ctx_t *ctx = netc_ctx_create(dict, &cfg);

/* Returns 1=generic, 2=sse42, 3=avx2, 4=neon. Never 0 for a valid ctx. */
uint8_t level = netc_ctx_simd_level(ctx);
printf("SIMD: %u\n", level);  /* e.g. 3 on an AVX2 machine */
```

### Speed Mode (`NETC_CFG_FLAG_FAST_COMPRESS`)

For latency-sensitive workloads where throughput matters more than optimal ratio:

```c
netc_cfg_t cfg = {
    .flags = NETC_CFG_FLAG_TCP_MODE | NETC_CFG_FLAG_DELTA
           | NETC_CFG_FLAG_COMPACT_HDR
           | NETC_CFG_FLAG_FAST_COMPRESS,  /* skip trial passes */
};
netc_ctx_t *enc = netc_ctx_create(dict, &cfg);

/* Decoder does NOT need the flag — output is fully compatible */
netc_cfg_t dec_cfg = {
    .flags = NETC_CFG_FLAG_TCP_MODE | NETC_CFG_FLAG_DELTA
           | NETC_CFG_FLAG_COMPACT_HDR,
};
netc_ctx_t *dec = netc_ctx_create(dict, &dec_cfg);
```

`NETC_CFG_FLAG_FAST_COMPRESS` skips expensive trial passes on the encoder side:
- PCTX vs single-region comparison is skipped (PCTX always used for multi-bucket packets)
- Delta-vs-LZP comparison trial is skipped
- LZ77 is skipped for packets < 512B (vs default < 256B)

Typical trade-off: **8-62% throughput gain, 0-10% ratio regression.** The decompressor is
unaffected and does not need this flag set. Compressed output is fully interoperable.

---

## Algorithm Pipeline

```
Packet Input (8-65535 bytes)
    |
    +-- high entropy? --> Passthrough (verbatim + header)
    |
    v
[Stage 1] Delta Prediction (stateful, opt-in)
    |  XOR for flags/floats, subtraction for counters
    v
[Stage 2] LZP XOR Pre-Filter (when dict has LZP table)
    |  hash(prev_byte, position) -> predicted_byte
    |  XOR with prediction: correct predictions -> 0x00
    |  Delta-vs-LZP comparison: picks smaller of delta+tANS vs LZP+tANS
    v
[Stage 3] tANS Entropy Coding
    |  Adaptive 12-bit (4096) or 10-bit (1024) tables, branch-free decode
    |  10-bit tables for ≤128B packets (lower per-symbol overhead)
    |  4 context buckets: HEADER/SUBHEADER/BODY/TAIL
    |  Per-position or bigram context variants
    v
[Stage 4] Multi-Codec Competition
    |  tANS vs LZ77 vs RLE vs passthrough -- smallest wins
    v
Compressed output (2-4B compact header + payload)
```

**Why ANS over Huffman?** Fractional-bit precision vs. integer-bit rounding. 5-15% better ratio on skewed byte distributions typical of game packets.

**Why LZP pre-filter?** Position-aware prediction captures per-offset byte distributions in structured packets. Correctly predicted bytes become 0x00, concentrating the distribution for tANS.

See [algorithm-decisions.md](docs/design/algorithm-decisions.md) for the full decision log.

---

## Benchmark Workloads

| ID | Description | Size | Entropy |
|----|-------------|-----:|--------:|
| WL-001 | Game state (position/velocity/flags) | 64B | ~3.2 bits/B |
| WL-002 | Game entity state (extended) | 128B | ~3.8 bits/B |
| WL-003 | Full entity snapshot | 256B | ~4.2 bits/B |
| WL-004 | Financial tick (price, volume, timestamp) | 32B | ~2.8 bits/B |
| WL-005 | Telemetry/sensor aggregation | 512B | ~4.5 bits/B |
| WL-006 | Random data (incompressible) | 128B | ~8.0 bits/B |
| WL-007 | Highly repetitive (zeros, patterns) | 128B | ~0.5 bits/B |
| WL-008 | Mixed traffic (60% WL-001 + others) | var | mixed |

---

## Project Structure

```
netc/
+-- include/
|   +-- netc.h                  # Single public C API header
+-- src/
|   +-- core/                   # Context, dictionary, compress, decompress
|   +-- algo/                   # tANS codec, delta prediction, LZP
|   +-- simd/                   # SSE4.2, AVX2, generic fallback
|   +-- util/                   # CRC32, bitstream I/O, platform helpers
+-- bench/                      # Benchmark harness
|   +-- bench_netc.c            # netc adapter
|   +-- bench_lz4.c             # LZ4 adapter
|   +-- bench_zstd.c            # Zstd adapter
|   +-- bench_zlib.c            # zlib adapter
|   +-- bench_snappy.c          # Snappy adapter
|   +-- bench_oodle.c           # OodleNetwork adapter
|   +-- bench_huffman.c         # Static Huffman adapter
|   +-- bench_corpus.c          # Workload generators
|   +-- bench_runner.c          # Benchmark runner
|   +-- bench_reporter.c        # Output formatting (table/CSV/JSON)
+-- tests/                      # Unit tests + fuzz targets
+-- docs/
|   +-- PRD.md                  # Product Requirements Document
|   +-- rfc/                    # Protocol and benchmark RFCs
|   +-- design/                 # Algorithm decision log
+-- CMakeLists.txt
```

---

## Documentation

- [RFC-001 — Compression Protocol](docs/rfc/RFC-001-netc-compression-protocol.md) — Stream format, packet format, API, security model
- [RFC-002 — Benchmark Requirements](docs/rfc/RFC-002-benchmark-performance-requirements.md) — Targets, methodology, CI gates, workload definitions
- [PRD](docs/PRD.md) — Product requirements, phases, success metrics
- [Algorithm Decisions](docs/design/algorithm-decisions.md) — Design decision log

---

## Roadmap

| Feature | Status |
|---------|--------|
| tANS entropy coder (12-bit + 10-bit adaptive) | Done |
| Delta prediction | Done |
| LZP XOR pre-filter | Done |
| Bigram context model (8-class trained) | Done |
| Compact packet header | Done |
| SIMD (SSE4.2, AVX2) | Done |
| SIMD bench reporting + dict freq dispatch (`netc_ctx_simd_level`, `netc_simd_level_name`) | Done |
| Security hardening + fuzz | Done |
| Benchmark harness | Done |
| Compress throughput optimization (1.4-1.9× normal, `FAST_COMPRESS` for +8-62%) | Done |
| Adaptive cross-packet learning (tANS + LZP + order-2 delta) | Done |
| ARM NEON SIMD | Planned |
| Profile-Guided Optimization (PGO) | Evaluated (+2-4% Clang, inconsistent GCC) |
| C++ SDK (Unreal Engine 5) | Planned |
| C# SDK (Unity / Godot 4) | Planned |
| v0.1.0 release tag | Pending |

---

## Profile-Guided Optimization (PGO)

PGO was evaluated with GCC 13.3 and Clang 18.1 on Linux (WSL2 Ubuntu 24.04), using 500,000 profiling iterations across all 8 workloads.

### Results Summary

| Compiler | Compress Throughput | Decompress Throughput | Stability |
|----------|:-------------------:|:---------------------:|-----------|
| **Clang 18 PGO** | **+2-4%** avg, +6.9% best | **+1-5%** avg | Stable, recommended |
| GCC 13 PGO | -6% to +13% | +2-16% | Inconsistent, not recommended |

**Compression ratio is unchanged** — PGO optimizes branch prediction and code layout, not algorithm output.

### Why the Modest Gains?

The codebase already applies manual optimizations that overlap with PGO's benefits:
- `NETC_LIKELY`/`NETC_UNLIKELY` branch hints on all hot-path conditionals
- `NETC_PREFETCH` for memory prefetching in tight loops
- `NETC_INLINE` (`always_inline`) on all critical inner functions
- `-O3` baseline with loop unrolling and vectorization

### Using PGO (Optional, Clang Recommended)

```bash
# Step 1: Instrument
cmake -B build-pgo -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_BENCH=ON \
  -DNETC_PGO_INSTRUMENT=ON
cmake --build build-pgo -j$(nproc)

# Step 2: Profile (500k+ iterations recommended)
./build-pgo/bench/bench --compressor=netc --count=500000 --compact-hdr

# Step 3: Merge + Optimize
llvm-profdata merge -output=build-pgo/pgo_data/default.profdata build-pgo/pgo_data/*.profraw
cmake -B build-pgo -DNETC_PGO_INSTRUMENT=OFF -DNETC_PGO_OPTIMIZE=ON
cmake --build build-pgo --clean-first -j$(nproc)
```

---

## Use Cases

- **Game servers** — Entity state synchronization, bandwidth reduction for 64-256B packets
- **Financial trading** — Market data tick stream compression with deterministic latency
- **IoT / telemetry** — Sensor packet aggregation with low memory footprint
- **Simulation engines** — High-frequency state replication across nodes

---

## Contributing

Pull requests welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.
All contributions must pass tests and maintain coverage >= 95%.

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).

---

*Inspired by [OodleNetwork](https://www.radgametools.com/oodlenetwork.htm) (RAD Game Tools). netc is an independent open-source implementation with no affiliation to RAD Game Tools.*

---

## Technical References

### Asymmetric Numeral Systems (ANS / tANS / FSE)

| Reference | Used for |
|-----------|----------|
| Jarek Duda, *[Asymmetric numeral systems](https://arxiv.org/abs/1311.2540)*, arXiv:1311.2540, 2013 | ANS theory, tANS encode/decode algorithm |
| Yann Collet, *[FSE — Finite State Entropy](https://github.com/Cyan4973/FiniteStateEntropy)* | Spread function, table construction, practical reference |
| Yann Collet et al., *[Zstandard compression format](https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md)* (RFC 8878) | FSE bitstream format, sentinel alignment, normalization |
| Charles Bloom, *[ANS notes](http://cbloomrants.blogspot.com/2014/01/01-10-14-understanding-ans-1.html)*, 2014 | Encoder normalization, bit-flush precision |

### Comparative Compressors

| Library | Reference |
|---------|-----------|
| LZ4 | [github.com/lz4/lz4](https://github.com/lz4/lz4) — Yann Collet |
| Zstandard | [github.com/facebook/zstd](https://github.com/facebook/zstd) — Facebook / Yann Collet |
| zlib | [zlib.net](https://zlib.net) — Jean-loup Gailly & Mark Adler |
| Snappy | [github.com/google/snappy](https://github.com/google/snappy) — Google |
| OodleNetwork | [radgametools.com/oodlenetwork.htm](https://www.radgametools.com/oodlenetwork.htm) — RAD Game Tools (proprietary) |
