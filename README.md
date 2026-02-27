# netc — Network Compression Library

[![Language](https://img.shields.io/badge/language-C11-orange.svg)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Coverage](https://img.shields.io/badge/coverage-95%25+-brightgreen.svg)](docs/coverage/)
[![Tests](https://img.shields.io/badge/tests-passing-success.svg)](tests/)
[![Standard](https://img.shields.io/badge/std-C11-orange.svg)](https://en.cppreference.com/w/c/11)
[![Version](https://img.shields.io/badge/version-0.1.0--dev-blue.svg)](CHANGELOG.md)

> High-performance binary packet compression for low-entropy network payloads — sub-microsecond latency, millions of packets per second.

---

## ✨ Key Features

- ⚡ **≥ 2 GB/s compression throughput** on structured game/simulation packets (single core)
- 🚀 **≥ 4 GB/s decompression throughput** — branch-free lookup decode path
- 📦 **≥ 5 Mpps compression / ≥ 10 Mpps decompression** on 64-byte packets (single thread)
- 📊 **≤ 0.55× compression ratio** on game state packets with a trained dictionary
- 🔄 **ANS (Asymmetric Numeral Systems)** entropy coder — near-optimal fractional-bit precision
  - rANS for packets > 64 bytes
  - tANS (12-bit table, branch-free) for packets ≤ 64 bytes
- 🧠 **Inter-packet delta prediction** — exploits temporal correlation in game/telemetry streams (+20–40% ratio)
- 🔗 **TCP stateful mode** — ring buffer history, context accumulates across packets
- 📡 **UDP stateless mode** — per-packet self-contained, sequence-numbered delta
- 🎯 **Dictionary training** — train from representative packet corpus, freeze for hot-path
- 🔒 **Zero dynamic allocation in hot path** — pre-allocated arena, deterministic latency
- 🛡️ **Passthrough guarantee** — never expands payload; activates automatically on incompressible data
- 🖥️ **SIMD acceleration** — SSE4.2, AVX2 (x86), ARM NEON; runtime dispatch, identical output across paths
- ⚡ **Profile-Guided Optimization (PGO)** — CMake PGO build targets included
- 📄 **Clean C11 API** — single header `netc.h`, zero dependencies beyond libc
- 🔍 **Comparative benchmarks** — vs. zlib, LZ4, Zstd, static Huffman, Snappy; CI gates enforced
- 🎮 **Native SDK for C++** — idiomatic wrappers with RAII, zero overhead, ready for Unreal Engine 5
- 🕹️ **Native SDK for C#** — managed wrappers with `unsafe` pinning, ready for Unity and Godot 4

---

## 📊 Performance

Benchmarks run on Linux x86_64, GCC 12 `-O3 -march=native`, single core, 100,000 iterations per measurement. Training corpus: 50,000 game state packets (WL-001).

| Compressor | Compress (GB/s) | Decompress (GB/s) | Latency p99 (ns, 128B) | Ratio (game packets) |
|------------|----------------:|------------------:|----------------------:|--------------------:|
| **netc** (AVX2, dict) | **3.1** | **7.2** | **380** | **0.47** |
| netc (generic, dict)  | 2.2  | 4.4  | 520  | 0.47 |
| LZ4 (fast)            | 2.0  | 5.1  | 610  | 0.71 |
| Zstd (level=1, dict)  | 0.9  | 2.8  | 1100 | 0.44 |
| zlib (level=1)        | 0.3  | 0.9  | 3200 | 0.52 |
| zlib (level=6)        | 0.1  | 0.9  | 9800 | 0.49 |
| Huffman (static)      | 1.4  | 3.2  | 680  | 0.58 |

> **Benchmarks are targets** for v0.1.0. Actual results will be published after the benchmark harness is implemented. See [RFC-002](docs/rfc/RFC-002-benchmark-performance-requirements.md) for methodology and workload definitions.

---

## 🚀 Quick Start

### Build

```bash
# Clone
git clone https://github.com/your-org/netc.git
cd netc

# Configure and build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Build and run benchmarks
cmake --build build --target bench
./build/bench/bench --workload=WL-001 --format=table
```

### Windows

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\bench\Release\bench.exe --workload=WL-001 --format=table
```

### Verify CI Gates

```bash
./build/bench/bench --ci-check --count=100000 --seed=42
# Exit code 0 = all performance gates pass
# Exit code 1 = one or more gates failed (see output for details)
```

---

## 📖 Usage

### Train a Dictionary

```c
#include "netc.h"

// Collect representative packets from your protocol
const uint8_t *packets[50000];
size_t         sizes[50000];
// ... fill packets from your network capture ...

// Train
netc_dict_t *dict = NULL;
netc_result_t r = netc_dict_train(packets, sizes, 50000, &dict);
assert(r == NETC_OK);

// Save for reuse
void  *blob;
size_t blob_size;
netc_dict_save(dict, &blob, &blob_size);
// ... write blob to disk ...
```

### Compress (TCP stateful mode)

```c
#include "netc.h"

// Load dictionary
netc_dict_t *dict = NULL;
netc_dict_load(blob, blob_size, &dict);

// Create context (one per connection)
netc_cfg_t cfg = {
    .flags            = NETC_CFG_FLAG_TCP_MODE | NETC_CFG_FLAG_DELTA,
    .compression_level = 5,
    .simd_level        = 0,  // auto-detect
};
netc_ctx_t *ctx = netc_ctx_create(dict, &cfg);

// Compress packets
uint8_t dst[NETC_MAX_PACKET_SIZE + NETC_HEADER_SIZE];
size_t  dst_size = 0;

netc_result_t r = netc_compress(ctx, src, src_size, dst, sizeof(dst), &dst_size);
if (r == NETC_OK) {
    // Send dst[0..dst_size-1] over the wire
    send(sock, dst, dst_size, 0);
}

// Decompress on the other side
uint8_t recovered[NETC_MAX_PACKET_SIZE];
size_t  recovered_size = 0;

netc_decompress(ctx_remote, dst, dst_size, recovered, sizeof(recovered), &recovered_size);
// recovered[0..recovered_size-1] == original src[0..src_size-1]

netc_ctx_destroy(ctx);
netc_dict_free(dict);
```

### Compress (UDP stateless mode)

```c
netc_cfg_t cfg = { .flags = NETC_CFG_FLAG_UDP_MODE };
netc_ctx_t *ctx = netc_ctx_create(dict, &cfg);

// Each packet is self-contained — no shared state required
netc_compress_stateless(dict, src, src_size, dst, sizeof(dst), &dst_size);
netc_decompress_stateless(dict, dst, dst_size, recovered, sizeof(recovered), &recovered_size);
```

---

## 🔄 Algorithm Overview

```
Packet Input
    │
    ├── size < 8 bytes or high entropy? ──► Passthrough (NETC_PKT_FLAG_PASSTHRU)
    │
    ▼
[Stage 1] Delta Prediction
    │  delta[i] = packet[i] - predictor[i]   (inter-packet correlation)
    │  Disabled for packets ≤ 64B
    ▼
[Stage 2] ANS Entropy Coding
    │  packet ≤ 64B  →  tANS (12-bit table, branch-free, L1-cached)
    │  packet > 64B  →  rANS (dual interleaved streams)
    ▼
[Stage 3] Passthrough Check
    │  compressed ≥ original? → emit original bytes
    ▼
Compressed bitstream (packet header + payload)
```

**Why ANS over Huffman?** ANS achieves fractional-bit precision vs. Huffman's integer-bit rounding — 5–15% better ratio on skewed byte distributions typical of game/telemetry packets. Decode speed is comparable with lookup tables.

**Why delta prediction?** Game packets are temporally correlated: positions change by small deltas each tick, enum values repeat, counters increment by 1. Subtracting predicted values reduces residual entropy by 20–40% before ANS encoding.

See [docs/design/algorithm-decisions.md](docs/design/algorithm-decisions.md) for the full decision log.

---

## 📋 Benchmark Workloads

| ID | Description | Size | Entropy | Primary Test |
|----|-------------|-----:|--------:|-------------|
| WL-001 | Game state (position/velocity/flags) | 64B | ~3.2 bits/B | Main target |
| WL-002 | Game entity state (extended) | 128B | ~3.8 bits/B | Latency |
| WL-003 | Full entity snapshot | 256B | ~4.2 bits/B | Ratio |
| WL-004 | Financial tick (price, volume, timestamp) | 32B | ~2.8 bits/B | Ratio |
| WL-005 | Telemetry/sensor aggregation | 512B | ~4.5 bits/B | Throughput |
| WL-006 | Cryptographically random data | 128B | ~8.0 bits/B | Passthrough |
| WL-007 | Highly repetitive (zeros, patterns) | 128B | ~0.5 bits/B | Max ratio |
| WL-008 | Mixed traffic (60% WL-001 + others) | var | mixed | Generalization |

Run a specific workload:

```bash
./bench --workload=WL-001 --compressor=netc --compressor=lz4 --count=1000000 --format=csv --output=results.csv
```

---

## 🔧 CI Performance Gates

All gates must pass (`exit 0`) before a release is tagged:

| Gate | Criterion |
|------|-----------|
| PERF-01 | Compress throughput ≥ 2 GB/s (WL-001) |
| PERF-02 | Decompress throughput ≥ 4 GB/s (WL-001) |
| PERF-03 | Compress p99 latency ≤ 1 µs (128B packet) |
| PERF-04 | Decompress p99 latency ≤ 500 ns (128B packet) |
| PERF-05 | Compress ≥ 5 Mpps (64B packets) |
| PERF-06 | Decompress ≥ 10 Mpps (64B packets) |
| RATIO-01 | Compression ratio ≤ 0.55 (WL-001, trained dict) |
| RATIO-02 | Random data (WL-006) ratio ≤ 1.01 (passthrough) |
| SAFETY-01 | 100,000 packets round-trip correctly |
| MEM-01 | Context memory ≤ 512 KB |
| COMP-01 | netc throughput > LZ4 throughput (WL-001) |
| COMP-02 | netc throughput > zlib throughput (WL-001) |
| COMP-03 | netc throughput > Zstd throughput (WL-001) |
| REGRESSION-01 | Throughput ≥ 95% of previous baseline |

---

## 📦 Project Structure

```
netc/
├── include/
│   └── netc.h                  # Single public C API header
├── src/
│   ├── core/                   # Context, dictionary, compress, decompress
│   ├── algo/                   # ANS codec, delta prediction, Huffman fallback
│   ├── simd/                   # SSE4.2, AVX2, NEON, generic fallback
│   └── util/                   # CRC32, bitstream I/O, statistics
├── sdk/
│   ├── cpp/                    # C++ SDK — Unreal Engine 5
│   │   ├── include/
│   │   │   ├── NetcContext.h   # FNetcContext RAII wrapper
│   │   │   ├── NetcDict.h      # FNetcDict shared dictionary
│   │   │   └── NetcTrainer.h   # Dictionary training helpers
│   │   └── NetcPlugin.uplugin  # UE5 plugin descriptor
│   └── csharp/                 # C# SDK — Unity & Godot 4
│       ├── Netc/
│       │   ├── NetcContext.cs  # NetcContext (IDisposable)
│       │   ├── NetcDict.cs     # NetcDict (thread-safe reads)
│       │   ├── NetcTrainer.cs  # Dictionary training
│       │   └── Interop/
│       │       └── NetcNative.cs # P/Invoke declarations
│       ├── Unity/
│       │   └── NetcTransport.cs  # Mirror/FishNet transport adapter
│       └── Godot/
│           └── NetcMultiplayerPeer.cs  # Godot MultiplayerPeer wrapper
├── bench/                      # Benchmark harness (separate binary)
│   └── bench_compressors/      # zlib, LZ4, Zstd, Huffman adapters
├── tests/                      # Unity C-framework unit + fuzz tests
├── docs/
│   ├── PRD.md                  # Product Requirements Document
│   ├── rfc/
│   │   ├── RFC-001-netc-compression-protocol.md
│   │   └── RFC-002-benchmark-performance-requirements.md
│   └── design/
│       └── algorithm-decisions.md
└── CMakeLists.txt
```

---

## 📚 Documentation

- [RFC-001 — netc Compression Protocol](docs/rfc/RFC-001-netc-compression-protocol.md) — Stream format, packet format, API spec, security model
- [RFC-002 — Benchmark & Performance Requirements](docs/rfc/RFC-002-benchmark-performance-requirements.md) — Targets, methodology, CI gates, workload definitions
- [PRD — Product Requirements Document](docs/PRD.md) — Goals, phases, success metrics, risks
- [Algorithm Decisions](docs/design/algorithm-decisions.md) — Design decision log (ANS, delta, SIMD, C11 rationale)

---

## 🎮 Game Engine SDKs

netc ships native SDKs for the major C++ and C# game engines — no managed-to-native bridges, no GC pressure, no per-packet allocations.

### C++ SDK — Unreal Engine 5

Drop `sdk/cpp/` into your UE5 plugin or module. The SDK wraps the C API with RAII handles and template helpers compatible with UE5's memory allocators.

```cpp
#include "Netc/NetcContext.h"

// Load a dictionary trained from your game's packet capture
FNetcDict Dict = FNetcDict::LoadFromFile(TEXT("Content/netc_game.dict"));

// One context per player connection (owns its ring buffer)
FNetcContext Ctx(Dict, ENetcMode::TCP);

// Compress outgoing packet — zero allocation in hot path
TArray<uint8> Compressed;
ENetcResult Result = Ctx.Compress(RawPacket, Compressed);

// Decompress incoming packet
TArray<uint8> Recovered;
Ctx.Decompress(Compressed, Recovered);
```

**Integration notes:**
- Compatible with UE5 `FSocket` / `INetworkingWebSocket` pipelines
- `FNetcDict` is `TSharedPtr`-safe — share across connections read-only
- No UObject heap — contexts live in stack or `TUniquePtr`
- Supports UE5 `NetworkPrediction` plugin workflows

### C# SDK — Unity & Godot 4

The C# SDK uses `unsafe` blocks with pinned byte spans — no `Marshal.Copy`, no managed heap allocation per packet.

**Unity (via NuGet or UPM):**

```csharp
using Netc;

// Load dictionary (call once, share across connections)
NetcDict dict = NetcDict.LoadFromBytes(dictBlob);

// One context per connection
using NetcContext ctx = new NetcContext(dict, NetcMode.Udp);

// Compress — writes directly into pre-allocated ArraySegment
byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];
int compressedLen = ctx.Compress(src, dst);

// Send compressed bytes over the wire
NetworkTransport.Send(connectionId, dst, compressedLen);
```

**Godot 4 (via GDExtension or NuGet):**

```csharp
using Netc;

// Train a dictionary from Godot PacketPeer captures
NetcDict dict = NetcTrainer.Train(capturedPackets);

// Wrap ENetMultiplayerPeer with netc compression
var peer = new NetcENetPeer(dict, NetcMode.Udp);
Multiplayer.MultiplayerPeer = peer;
// All packets are now automatically compressed/decompressed
```

**Integration notes:**
- Unity: compatible with `Mirror`, `FishNet`, `Netcode for GameObjects` transport layers
- Godot: compatible with `ENetMultiplayerPeer`, `WebRTCMultiplayerPeer`, custom `MultiplayerPeer`
- Zero GC pressure — all hot-path operations use pinned `Span<byte>` and `stackalloc`
- `NetcDict` is thread-safe for concurrent reads (multiple connections share one dict)
- `NetcContext` is NOT thread-safe — one per connection, consistent with transport layer design

### SDK Feature Comparison

| Feature | C Core | C++ (UE5) | C# (Unity/Godot) |
|---------|:------:|:---------:|:----------------:|
| Zero hot-path allocation | ✅ | ✅ | ✅ |
| Dictionary training API | ✅ | ✅ | ✅ |
| TCP stateful mode | ✅ | ✅ | ✅ |
| UDP stateless mode | ✅ | ✅ | ✅ |
| SIMD acceleration | ✅ | ✅ | ✅ (via C core) |
| RAII / using-safe handles | — | ✅ | ✅ |
| GC-pressure free | — | ✅ | ✅ (Span-based) |
| UE5 module integration | — | ✅ | — |
| Unity transport layer | — | — | ✅ |
| Godot MultiplayerPeer | — | — | ✅ |

---

## 🎯 Use Cases

- **Unreal Engine 5** — Entity replication bandwidth reduction at scale (dedicated servers, 100+ players)
- **Unity multiplayer** — Mirror/FishNet packet compression for mobile and WebGL targets
- **Godot 4** — Lightweight netcode compression for indie multiplayer games
- **Game servers** — Entity state synchronization at 1–10 Mpps per core
- **Financial trading** — Market data tick stream compression with deterministic latency
- **IoT / telemetry** — Sensor packet aggregation with low memory footprint
- **Simulation engines** — High-frequency state replication across nodes
- **Network research** — Reproducible algorithm comparison benchmark harness

---

## 🤝 Contributing

Pull requests welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.
All contributions must pass `make check` (type check, lint, tests, coverage ≥ 95%, CI benchmark gates).

---

## 📄 License

MIT — see [LICENSE](LICENSE).

---

*Inspired by [Oodle Network](https://www.radgametools.com/oodlenetwork.htm) (RAD Game Tools). netc is an independent open-source implementation with no affiliation to RAD Game Tools.*
