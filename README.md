# netc — Network Compression Library

[![Language](https://img.shields.io/badge/language-C11-orange.svg)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Coverage](https://img.shields.io/badge/coverage-95%25+-brightgreen.svg)](tests/)
[![Version](https://img.shields.io/badge/version-0.1.0--dev-blue.svg)](CHANGELOG.md)

> High-performance binary packet compression for low-entropy network payloads. Purpose-built for game netcode, telemetry, and real-time protocols.

---

## Key Features

- **tANS (FSE) entropy coder** — 12-bit table (4096 entries), branch-free decode, fractional-bit precision
- **LZP prediction pre-filter** — position-aware order-1 context XOR filter, predicted bytes become 0x00
- **Bigram context model** — order-1 frequency tables per context bucket
- **Inter-packet delta prediction** — field-class aware (XOR for flags/floats, subtraction for counters)
- **Compact packet header** — 2B header for packets ≤ 127B, 4B for larger. Opt-in via `NETC_CFG_FLAG_COMPACT_HDR`
- **ANS state compaction** — 2B tANS state in compact mode (vs. 4B legacy)
- **Multi-codec competition** — tANS vs LZ77 vs RLE vs passthrough per packet, smallest wins
- **Dictionary training** — train from packet corpus, freeze for hot-path. v4 format with LZP + bigram tables
- **Stateful & stateless modes** — ring buffer history (TCP) or self-contained per-packet (UDP)
- **SIMD acceleration** — SSE4.2 and AVX2 (x86) with runtime dispatch, generic scalar fallback
- **Zero dynamic allocation in hot path** — pre-allocated arena, deterministic latency
- **Passthrough guarantee** — never expands payload; activates automatically on incompressible data
- **Security hardened** — bounds-checked decompressor, CRC32 dictionary validation, fuzz tested
- **Clean C11 API** — single header `netc.h`, zero dependencies beyond libc

---

## Benchmarks

### Compression Ratio — netc vs Compressors

Measured on Windows x86_64, MSVC `/O2`, 50,000 iterations, 10,000 training packets.
Lower is better (compressed size / original size).

| Compressor | WL-001 (64B) | WL-002 (128B) | WL-003 (256B) | Design goal |
|------------|:------------:|:-------------:|:-------------:|-------------|
| **netc** (compact header) | **0.765** | **0.591** | **0.349** | Network packets |
| **netc** (legacy 8B header) | 0.890 | 0.638 | 0.373 | Network packets |
| OodleNetwork 2.9.13 | 0.68 | 0.52 | 0.35 | Network packets |
| Zstd (level=1, dict) | ~0.85 | ~0.44 | ~0.30 | General purpose |
| zlib (level=1) | ~0.95 | ~0.52 | ~0.38 | General purpose |
| LZ4 (fast) | ~1.00+ | ~0.71 | ~0.55 | Speed-oriented |
| Snappy | ~1.00+ | ~0.78 | ~0.60 | Speed-oriented |

> **Note:** General-purpose compressors (Zstd, zlib) can beat netc on ratio for larger payloads (≥ 128B) because they have no per-packet header overhead and use more aggressive algorithms. netc's advantage is on small packets (32-128B) typical of game netcode, where per-packet overhead dominates and general-purpose compressors struggle. LZ4/Snappy often fail to compress small packets at all (ratio ≥ 1.0).

### netc Header Overhead Breakdown

| Mode | Header | ANS state | Total overhead | Best for |
|------|:------:|:---------:|:--------------:|----------|
| Compact (≤ 127B packet) | 2B | 2B | **4B** | Small game packets |
| Compact (128-65535B) | 4B | 2B | **6B** | Medium payloads |
| Legacy | 8B | 4B | **12B** | Compatibility |

### Throughput Targets

Performance targets for v1.0 on server-grade hardware (not yet measured on target hardware):

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

### Gap to OodleNetwork

| Workload | netc (compact) | Oodle 2.9.13 | Gap (bytes) | Gap (ratio) |
|----------|:--------------:|:------------:|:-----------:|:-----------:|
| WL-001 64B | 0.765 | 0.68 | ~5.4B | 0.085 |
| WL-002 128B | 0.591 | 0.52 | ~9.1B | 0.071 |
| WL-003 256B | 0.349 | 0.35 | **-0.3B** | **-0.001** |

WL-003 compact mode (0.349) now matches OodleNetwork (0.35). Remaining gap on small packets is dominated by per-packet header overhead (netc 4B min vs. Oodle 0B out-of-band). Closing the gap further is the primary focus for v1.0.

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
    |  12-bit table (4096 entries), branch-free decode
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
| tANS entropy coder | Done |
| Delta prediction | Done |
| LZP XOR pre-filter | Done |
| Bigram context model | Done |
| Compact packet header | Done |
| SIMD (SSE4.2, AVX2) | Done |
| Security hardening + fuzz | Done |
| Benchmark harness | Done |
| ARM NEON SIMD | Planned |
| Profile-Guided Optimization (PGO) | Planned (requires GCC/Clang) |
| C++ SDK (Unreal Engine 5) | Planned |
| C# SDK (Unity / Godot 4) | Planned |
| v0.1.0 release tag | Pending |

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

MIT — see [LICENSE](LICENSE).

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
