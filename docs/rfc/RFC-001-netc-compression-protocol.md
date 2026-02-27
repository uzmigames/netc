# RFC-001: netc — Network Compression Library for Low-Entropy Binary Payloads

```
Status:    DRAFT
Version:   0.1.0
Date:      2026-02-26
Authors:   netc Contributors
Language:  C (C11)
Target:    TCP/UDP network packet compression
Inspired:  Oodle Network (RAD Game Tools)
```

---

## Abstract

This document specifies **netc** (Network Compression), a C library designed to compress and decompress low-entropy binary payloads at wire speed for high-throughput network scenarios. netc targets millions of packets per second (Mpps) with latency budgets under 1 microsecond per packet, outperforming general-purpose compressors (zlib, LZ4, Zstd) for the specific case of structured game/simulation network packets.

---

## Table of Contents

1. [Motivation](#1-motivation)
2. [Scope](#2-scope)
3. [Terminology](#3-terminology)
4. [Architecture Overview](#4-architecture-overview)
5. [Compression Model](#5-compression-model)
6. [Core Algorithm: NETC-LowE](#6-core-algorithm-netc-lowe)
7. [Dictionary and Training](#7-dictionary-and-training)
8. [Stream Format](#8-stream-format)
9. [Packet Format](#9-packet-format)
10. [API Specification](#10-api-specification)
11. [Memory Model](#11-memory-model)
12. [Thread Safety](#12-thread-safety)
13. [Error Handling](#13-error-handling)
14. [Platform Requirements](#14-platform-requirements)
15. [Security Considerations](#15-security-considerations)
16. [References](#16-references)

---

## 1. Motivation

### 1.1 The Problem

Modern networked applications (game servers, financial systems, telemetry platforms, simulation engines) transmit millions of small binary packets per second. These packets share structural similarity:

- Fixed-size headers with predictable fields
- Delta-encoded position/state data (low entropy)
- Repeated enum values and bitmasks
- Floating-point values clustered in narrow ranges

General-purpose compressors fail this workload because:

| Compressor | Problem |
|------------|---------|
| zlib/deflate | ~200 MB/s throughput, ~3µs per small packet — too slow |
| LZ4 | Better throughput but no statistical modeling, poor ratio on structured data |
| Zstd | Excellent ratio but 1–5µs per packet, too much latency |
| Huffman (static) | No adaptation to packet correlation across time |
| Snappy | Optimized for throughput but not for micro-packet structured data |

### 1.2 The Oodle Network Reference

RAD Game Tools' [Oodle Network](https://www.radgametools.com/oodlenetwork.htm) demonstrates that dictionary-based Huffman coding trained on representative traffic achieves:
- 50–70% compression ratio on game packets
- Sub-microsecond decompression per packet
- Deterministic latency (no adaptive decision trees at decode time)

netc replicates this approach as an open, portable C library.

### 1.3 Why C

- Zero-overhead FFI from any language (Python, Rust, Go, C++, Zig)
- Direct SIMD intrinsics (SSE4.2, AVX2, NEON) without abstraction cost
- Predictable memory layout for cache performance
- Suitable for kernel-space and embedded targets

---

## 2. Scope

### 2.1 In Scope

- Compression/decompression of individual packets (8–1500 bytes typical)
- Dictionary training from representative packet captures
- Shared-state compression context for TCP streams
- Stateless per-packet compression for UDP
- Benchmarking harness comparing netc vs. zlib, LZ4, Zstd, Huffman
- C11 API with no dynamic allocation in hot path
- SIMD acceleration (SSE4.2, AVX2, ARM NEON)

### 2.2 Out of Scope

- Transport-layer implementation (TCP/UDP sockets)
- Encryption or authentication
- Packet framing/fragmentation
- Reliable delivery mechanisms

---

## 3. Terminology

| Term | Definition |
|------|------------|
| **Packet** | A single binary payload to be compressed/decompressed, typically 8–1500 bytes |
| **Context** | A compression state holding dictionary, probability tables, and ring buffer |
| **Dictionary** | Pre-trained byte-frequency and bigram tables derived from representative packets |
| **Low-entropy payload** | Binary data with predictable byte distribution (structured protocol data) |
| **Mpps** | Millions of packets per second |
| **NETC-LowE** | netc's primary compression algorithm (Low-Entropy optimized) |
| **Training corpus** | A representative sample of packets used to build the dictionary |
| **Throughput** | Compression/decompression speed in MB/s or Mpps |
| **Latency** | Per-packet compression or decompression time in nanoseconds |

---

## 4. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                        netc Library                           │
│                                                              │
│  ┌─────────────┐   ┌─────────────┐   ┌───────────────────┐  │
│  │  Training   │   │  Compress   │   │    Decompress     │  │
│  │   Engine    │   │   Engine    │   │      Engine       │  │
│  │             │   │             │   │                   │  │
│  │ - Corpus    │   │ - NETC-LowE │   │ - NETC-LowE       │  │
│  │   analysis  │──▶│ - Dict lookup│  │ - Dict lookup     │  │
│  │ - Huffman   │   │ - Huffman   │   │ - Huffman decode  │  │
│  │   table gen │   │   encode    │   │ - Output buffer   │  │
│  │ - Bigram    │   │ - Bitstream │   │                   │  │
│  │   model     │   │   packing   │   │                   │  │
│  └─────────────┘   └─────────────┘   └───────────────────┘  │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                   Context Manager                        │ │
│  │  - Per-connection state (TCP)                            │ │
│  │  - Shared static context (UDP broadcast)                 │ │
│  │  - Memory pool (no malloc in hot path)                   │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │              SIMD Acceleration Layer                     │ │
│  │  SSE4.2 | AVX2 | NEON | Generic fallback                │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

### 4.1 Design Principles

1. **No allocation in hot path** — all memory pre-allocated at context creation
2. **Cache-friendly data layout** — compression tables fit in L1/L2 cache
3. **Branch-free decode** — lookup tables eliminate conditional branches
4. **SIMD everywhere possible** — bulk operations use vector instructions
5. **Deterministic latency** — no adaptive algorithms during encode/decode

---

## 5. Compression Model

### 5.1 Target Data Characteristics

netc is optimized for packets with these properties:

| Property | Typical Range |
|----------|---------------|
| Packet size | 8 – 1500 bytes |
| Byte entropy | 1.5 – 5.0 bits/byte (vs. 8 for random) |
| Inter-packet correlation | High (same game tick, same protocol) |
| Byte distribution | Skewed (few values dominate) |
| Structural regularity | Fixed header layout, variable payload |

### 5.2 Algorithm Selection

NETC-LowE uses a three-stage pipeline:

```
Input packet
    │
    ▼
[Stage 1: Delta Encoding]
    │ Subtract previous packet bytes (inter-packet correlation)
    │ or use structural prediction (header field delta)
    ▼
[Stage 2: Entropy Coding — Asymmetric Numeral Systems (ANS)]
    │ rANS (range ANS) for bulk data
    │ tANS (table ANS) for small packets < 64 bytes
    ▼
[Stage 3: Huffman Post-Pass (optional)]
    │ Applied only when ANS ratio < threshold
    ▼
Compressed bitstream
```

### 5.3 Why ANS over Huffman

| Property | Huffman | rANS |
|----------|---------|------|
| Optimal for symbol prob | Integer bits (suboptimal) | Fractional bits (near-optimal) |
| Decode speed | Table lookup, ~1 ns/symbol | ~1.5 ns/symbol |
| Compression ratio | Good | 5–15% better than Huffman |
| Code complexity | Low | Moderate |
| State size | None (stateless) | 64-bit state |
| Parallelizability | Bit-serial | Interleaved streams possible |

For packets under 64 bytes, tANS (tabular ANS) is used with pre-computed decode tables.

---

## 6. Core Algorithm: NETC-LowE

### 6.1 Delta Prediction

Before entropy coding, netc applies byte-level delta prediction:

```
delta[i] = packet[i] - predictor[i]
```

The predictor uses a weighted combination:
- Previous packet at same byte offset (inter-packet delta, weight 0.7)
- Structural prediction from header schema (weight 0.3)

The delta residuals have significantly lower entropy than raw bytes, improving compression ratio by 20–40% for structured protocol data.

### 6.2 Context Mixing

For bytes with position-dependent distributions (headers vs. payload), netc uses context mixing:

- Byte position context (top 8 bits of position)
- Previous byte context (bigram model)
- Field-type context (inferred from training)

Each context maintains a separate probability model (256-entry frequency table).

### 6.3 ANS Coding

rANS state transition:

```c
// Encode symbol s with frequency freq[s], total M
state = (state / freq[s]) * M + cumfreq[s] + (state % freq[s]);

// Decode: given state x, find symbol s where cumfreq[s] <= (x % M) < cumfreq[s+1]
slot = x % M;
s = symbol_lookup[slot];  // O(1) table lookup
x = freq[s] * (x / M) + slot - cumfreq[s];
```

The ANS state is flushed to the output stream as a 32-bit or 64-bit word when it exceeds the normalization range.

### 6.4 Small Packet Optimization

For packets ≤ 64 bytes:
- Skip delta encoding (insufficient history)
- Use tANS with 12-bit table (4096 entries, fits in L1 cache)
- Output includes 2-byte tANS table index for decoder

For packets > 64 bytes:
- Apply full NETC-LowE pipeline
- Use rANS with interleaved dual streams for higher throughput

---

## 7. Dictionary and Training

### 7.1 Training Requirements

A training corpus MUST contain:
- Minimum 1,000 representative packets
- Packets from the same protocol/session type
- Representative distribution of game states/message types

### 7.2 Training Algorithm

```
1. Collect N packets into corpus buffer
2. Compute byte frequency table (256 entries, global)
3. Compute bigram frequency table (256×256 entries, per-position context)
4. Build ANS probability tables (normalized to power-of-2 total)
5. Serialize dictionary to binary blob (netc_dict_t)
6. Validate: compress/decompress all corpus packets, verify round-trip
```

### 7.3 Dictionary Format

```c
typedef struct {
    uint32_t magic;           // 0x4E455443 ('NETC')
    uint16_t version;         // Dictionary format version
    uint16_t flags;           // NETC_DICT_FLAG_*
    uint32_t corpus_size;     // Number of training packets
    uint32_t dict_size;       // Size of this struct in bytes
    uint8_t  freq[256];       // Normalized byte frequencies (ANS)
    uint16_t bigram[256][256]; // Position-context bigram model (optional)
    uint32_t checksum;        // CRC32 of preceding data
} netc_dict_t;
```

### 7.4 Dictionary Constraints

- Maximum dictionary size: 512 KB
- Minimum dictionary size: 1 KB (byte frequency table only)
- Dictionary is immutable after training (read-only during compression)
- Multiple dictionaries can be active simultaneously (per-connection)

---

## 8. Stream Format

### 8.1 TCP Stream

For TCP connections, a shared compression context accumulates history:

```
┌──────────┬──────────────────────────────────────────────┐
│  Stream  │ [Header][Packet0][Packet1]...[PacketN]        │
│  Header  │                                              │
│  (16B)   │                                              │
└──────────┴──────────────────────────────────────────────┘

Stream Header:
  [0..3]  magic = 0x4E455443
  [4..5]  version
  [6..7]  flags
  [8..11] dict_id (reference to negotiated dictionary)
  [12..15] reserved
```

### 8.2 UDP Datagram

Each UDP packet is self-contained (no shared state by default):

```
┌────────────────────────────────────────────────────────┐
│ [2B flags][2B orig_size][compressed payload]            │
└────────────────────────────────────────────────────────┘

Flags:
  bit 0: NETC_FLAG_COMPRESSED (0=uncompressed passthrough)
  bit 1: NETC_FLAG_DICT_PRESENT (dict_id follows flags)
  bit 2: NETC_FLAG_DELTA (delta against previous packet in sequence)
  bit 3-15: reserved
```

### 8.3 Passthrough Mode

If compressed size ≥ original size, netc emits the original uncompressed payload with `NETC_FLAG_COMPRESSED = 0`. This guarantees netc never increases payload size.

---

## 9. Packet Format

### 9.1 Compressed Packet Layout

```
Offset  Size  Field
──────  ────  ─────────────────────────────────────────────
0       2     original_size  (uint16, max 65535)
2       2     compressed_size (uint16)
4       1     flags          (NETC_PKT_FLAG_*)
5       1     algorithm      (NETC_ALG_ANS | NETC_ALG_HUFFMAN | NETC_ALG_PASSTHRU)
6       2     context_seq    (rolling sequence for delta, UDP mode)
8       N     compressed_payload
```

### 9.2 Flags

```c
#define NETC_PKT_FLAG_DELTA      0x01  // Delta-encoded from previous packet
#define NETC_PKT_FLAG_BIGRAM     0x02  // Bigram context model active
#define NETC_PKT_FLAG_SMALL      0x04  // Small packet path (tANS)
#define NETC_PKT_FLAG_PASSTHRU   0x08  // Uncompressed passthrough
#define NETC_PKT_FLAG_DICT_ID    0x10  // Explicit dict_id present in header
```

---

## 10. API Specification

### 10.1 Context Lifecycle

```c
// Create compression context
netc_ctx_t *netc_ctx_create(const netc_dict_t *dict, const netc_cfg_t *cfg);

// Destroy and free context
void netc_ctx_destroy(netc_ctx_t *ctx);

// Reset context state (keep dictionary)
void netc_ctx_reset(netc_ctx_t *ctx);
```

### 10.2 Dictionary Management

```c
// Train dictionary from corpus
netc_result_t netc_dict_train(
    const uint8_t **packets,   // Array of packet pointers
    const size_t  *sizes,      // Array of packet sizes
    size_t         count,      // Number of packets
    netc_dict_t  **out_dict    // Output dictionary (allocated by netc)
);

// Load dictionary from binary blob
netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out);

// Save dictionary to binary blob
netc_result_t netc_dict_save(const netc_dict_t *dict, void **out, size_t *out_size);

// Free dictionary
void netc_dict_free(netc_dict_t *dict);
```

### 10.3 Compression

```c
// Compress single packet
netc_result_t netc_compress(
    netc_ctx_t   *ctx,
    const void   *src,        // Input packet
    size_t        src_size,   // Input size (bytes)
    void         *dst,        // Output buffer
    size_t        dst_cap,    // Output buffer capacity
    size_t       *dst_size    // Actual output size
);

// Decompress single packet
netc_result_t netc_decompress(
    netc_ctx_t   *ctx,
    const void   *src,
    size_t        src_size,
    void         *dst,
    size_t        dst_cap,
    size_t       *dst_size
);

// Stateless compress (no context, UDP mode)
netc_result_t netc_compress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);

// Stateless decompress
netc_result_t netc_decompress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);
```

### 10.4 Configuration

```c
typedef struct {
    uint32_t flags;             // NETC_CFG_FLAG_*
    size_t   ring_buffer_size;  // History ring buffer (default: 64KB)
    uint8_t  compression_level; // 0=fastest, 9=best ratio (default: 5)
    uint8_t  simd_level;        // 0=auto, 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON
    size_t   arena_size;        // Pre-allocated memory arena (0=use default)
} netc_cfg_t;

#define NETC_CFG_FLAG_TCP_MODE    0x01  // Stateful TCP compression
#define NETC_CFG_FLAG_UDP_MODE    0x02  // Stateless UDP compression
#define NETC_CFG_FLAG_DELTA       0x04  // Enable inter-packet delta
#define NETC_CFG_FLAG_BIGRAM      0x08  // Enable bigram context model
#define NETC_CFG_FLAG_STATS       0x10  // Collect compression statistics
```

### 10.5 Return Codes

```c
typedef enum {
    NETC_OK                = 0,
    NETC_ERR_NOMEM         = -1,  // Allocation failure
    NETC_ERR_TOOBIG        = -2,  // Input exceeds NETC_MAX_PACKET_SIZE
    NETC_ERR_CORRUPT       = -3,  // Corrupt compressed data
    NETC_ERR_DICT_INVALID  = -4,  // Dictionary checksum mismatch
    NETC_ERR_BUF_SMALL     = -5,  // Output buffer too small
    NETC_ERR_CTX_NULL      = -6,  // NULL context pointer
    NETC_ERR_UNSUPPORTED   = -7,  // Algorithm/feature not supported
    NETC_ERR_VERSION       = -8,  // Dictionary version mismatch
} netc_result_t;
```

---

## 11. Memory Model

### 11.1 Pre-Allocation

All compression state is allocated at `netc_ctx_create()`. The hot path (compress/decompress) performs **zero dynamic allocations**.

### 11.2 Memory Layout

```
netc_ctx_t {
    netc_dict_t   *dict;           // Read-only, shared
    uint8_t        ring[RING_SZ];  // History ring buffer
    uint32_t       ring_pos;       // Current ring position
    netc_freq_t    freq[256];      // ANS frequency tables
    uint32_t       ans_state;      // Current ANS encoder state
    uint8_t       *arena;          // Pre-allocated scratch space
    size_t         arena_size;
    netc_stats_t   stats;          // Optional statistics (if NETC_CFG_FLAG_STATS)
}
```

### 11.3 Arena

The arena is used for temporary working buffers (delta buffer, ANS output staging). Default size: 2× max packet size = 3000 bytes. Can be provided by caller for zero-copy operation.

---

## 12. Thread Safety

- `netc_ctx_t` is **NOT thread-safe**. Each thread MUST have its own context.
- `netc_dict_t` IS thread-safe for concurrent reads. Training is single-threaded.
- Multiple contexts may share the same `netc_dict_t` (read-only).

---

## 13. Error Handling

All functions return `netc_result_t`. On error:
- Output buffers are left unmodified
- Context state is rolled back to pre-call state
- Error details available via `netc_strerror(result)`

```c
const char *netc_strerror(netc_result_t result);
```

---

## 14. Platform Requirements

| Requirement | Value |
|-------------|-------|
| C standard | C11 minimum |
| Pointer size | 32-bit or 64-bit |
| Endianness | Little-endian primary, big-endian supported |
| Compiler | GCC 9+, Clang 10+, MSVC 2019+ |
| SIMD (optional) | SSE4.2, AVX2 (x86), NEON (ARM) |
| OS | Linux, Windows, macOS, FreeBSD |
| Dependencies | None (libc only) |

### 14.1 SIMD Detection

At context creation, netc auto-detects SIMD capabilities via CPUID (x86) or AT_HWCAP (Linux/ARM). The appropriate implementation is selected at runtime. All paths produce identical output.

---

## 15. Security Considerations

### 15.1 Decompression Safety

The decompressor MUST:
- Validate `original_size` against `dst_cap` before decompressing
- Validate ANS state stays within bounds on every symbol decode
- Validate dictionary checksum before use
- Never read beyond `src_size` bytes of input
- Limit output to `dst_cap` bytes regardless of `original_size`

### 15.2 Denial of Service

A malicious sender COULD craft a packet claiming `original_size = 65535` with a tiny compressed payload. The decompressor MUST enforce the output buffer cap and MUST NOT trust `original_size` for memory allocation decisions.

### 15.3 Dictionary Poisoning

Dictionary loading MUST verify the CRC32 checksum. A corrupt or tampered dictionary results in `NETC_ERR_DICT_INVALID` and the dictionary is rejected.

---

## 16. References

1. RAD Game Tools, "Oodle Network Compression," https://www.radgametools.com/oodlenetwork.htm
2. Duda, J. (2013), "Asymmetric numeral systems," arXiv:1311.2540
3. RFC 1951 — DEFLATE Compressed Data Format Specification
4. Collet, Y. (2016), "Zstandard Compression," RFC 8478
5. LZ4 Compression Algorithm, https://lz4.github.io/lz4/
6. Mahoney, M. (2013), "Data Compression Explained," http://mattmahoney.net/dc/dce.html
7. Bloom, C. (2013), "On Compression and the Oodle Kraken Algorithm," RAD Game Tools Blog

---

*End of RFC-001*
