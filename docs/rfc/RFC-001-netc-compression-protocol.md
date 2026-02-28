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

netc is a **buffer-to-buffer compression layer**. It operates exclusively on caller-provided memory buffers (`const void *src` → `void *dst`). It has no knowledge of sockets, transports, framing protocols, or delivery guarantees. The caller decides whether the compressed output is sent over TCP, UDP, QUIC, shared memory, a ring buffer, a custom game protocol, or written to a file — netc does not care and does not constrain that choice.

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

- Compression/decompression of binary payloads (8–1500 bytes typical) from and to caller-provided buffers
- Dictionary training from representative payload captures
- **Stateful** compression context: caller accumulates inter-payload history across sequential calls (suited for ordered, reliable streams)
- **Stateless** compression: each call is self-contained, no shared state between calls (suited for independent datagrams)
- Benchmarking harness comparing netc vs. zlib, LZ4, Zstd, Huffman
- C11 API with no dynamic allocation in hot path
- SIMD acceleration (SSE4.2, AVX2, ARM NEON)

### 2.2 Out of Scope

- Transport-layer implementation (sockets, connections, framing)
- Transport protocol selection — netc output can be sent over TCP, UDP, QUIC, WebSocket, shared memory, IPC, or any other medium
- Encryption or authentication
- Packet framing, fragmentation, or reassembly
- Reliable delivery, retransmission, or ordering guarantees
- Buffer management beyond the caller-provided src/dst buffers

---

## 3. Terminology

| Term | Definition |
|------|------------|
| **Payload** | A single binary buffer to be compressed/decompressed, typically 8–1500 bytes. May originate from any source (network packet, game state struct, IPC message, file chunk, etc.) |
| **Context** | A compression state holding dictionary, probability tables, and history ring buffer. Owned and managed by the caller. |
| **Stateful mode** | A context that accumulates history across sequential `netc_compress` calls. Useful when payloads arrive in order with no loss (e.g., a reliable stream). |
| **Stateless mode** | Each `netc_compress_stateless` call is fully independent. No history is shared between calls. Useful for independent datagrams or when ordering/loss cannot be guaranteed. |
| **Dictionary** | Pre-trained byte-frequency and bigram tables derived from representative payload captures |
| **Low-entropy payload** | Binary data with predictable byte distribution (structured protocol data) |
| **Mpps** | Millions of payloads per second |
| **NETC-LowE** | netc's primary compression algorithm (Low-Entropy optimized) |
| **Training corpus** | A representative sample of payloads used to build the dictionary |
| **Throughput** | Compression/decompression speed in MB/s or Mpps |
| **Latency** | Per-payload compression or decompression time in nanoseconds |
| **Transport** | The mechanism used to move compressed bytes from producer to consumer. netc is transport-agnostic: TCP, UDP, QUIC, shared memory, ring buffer, or any other medium are all valid. |

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
│  │  - Stateful mode: history accumulates across calls       │ │
│  │  - Stateless mode: each call is self-contained           │ │
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

NETC-LowE uses a multi-stage pipeline:

```
Input packet
    │
    ▼
[Stage 1: Delta Prediction — field-class aware]
    │ Integer counters: byte subtraction from previous packet (wrapping)
    │ Float components / flags: XOR with previous packet byte
    │ Field class inferred from training corpus (statistical, not schema-based)
    │ Skipped for first packet or when delta is disabled
    ▼
[Stage 2: LZP XOR Pre-Filter — position-aware prediction]
    │ hash(previous_byte, byte_position) → predicted byte
    │ XOR each byte with prediction: correct predictions → 0x00
    │ Applied when dict has LZP table and delta was NOT applied
    │ tANS tables retrained on LZP-filtered data during training
    ▼
[Stage 3: Entropy Coding — tANS (FSE)]
    │ Single tANS codec for all packet sizes
    │ 12-bit table (4096 entries × 4 bytes = 16 KB, fits in L1 cache)
    │ Branch-free decode: one table lookup + one state transition per symbol
    │ Multi-region bucket selection or per-position context (PCTX)
    │ Optional bigram (order-1) context model
    ▼
[Stage 4: Competition]
    │ tANS vs LZ77 vs RLE vs passthrough — smallest output wins
    │ Passthrough if compressed_size ≥ original_size (see AD-006)
    ▼
Compressed bitstream (compact or legacy header + payload)
```

**Note on rANS**: rANS is available as an opt-in secondary path for v0.2+. See AD-001 in `docs/design/algorithm-decisions.md`.

### 5.3 Why tANS over Huffman / rANS

| Property | Huffman | rANS | tANS (FSE) — chosen |
|----------|---------|------|----------------------|
| Optimal for symbol prob | Integer bits (suboptimal) | Fractional bits (near-optimal) | Fractional bits (near-optimal) |
| Decode speed | Table lookup, ~1 ns/symbol | ~1.5 ns/symbol (division per symbol) | ~1 ns/symbol (no division) |
| Compression ratio | Good | 5–15% better than Huffman | 5–15% better than Huffman |
| Code complexity | Low | Moderate | Moderate |
| Encode hot path | O(1) per symbol | Division/modulo per symbol | Table lookup per symbol |
| Checkpoint size | Stateless | state + cursor + renorm (complex) | state + position (simple) |
| Prior art | Widespread | Zstd rANS streams | Zstd FSE (all entropy coding) |

---

## 6. Core Algorithm: NETC-LowE

### 6.1 Delta Prediction

Before entropy coding, netc applies **field-class aware** delta prediction. The delta strategy depends on the structural role of each byte, inferred statistically from the training corpus — not from an explicit schema.

| Field class | Examples | Delta strategy |
|-------------|----------|----------------|
| Integer counters | sequence numbers, tick IDs, entity IDs | Byte subtraction from previous packet (wrapping) |
| Enum / flags | message type, ability flags, bitmasks | XOR with previous packet byte |
| Float components | position.x/y/z, velocity, rotation | XOR with previous packet byte |
| Fixed strings / GUIDs | player ID, asset hash | Passthrough |
| Repeated constants | protocol version, static headers | Passthrough |

**Rationale**: Blind byte subtraction on IEEE 754 floats produces high-entropy residuals because the exponent bits cancel poorly. XOR preserves mantissa delta patterns which have lower entropy. See AD-002 in `docs/design/algorithm-decisions.md`.

Delta can be disabled per-packet (flag `NETC_PKT_FLAG_DELTA` unset) without changing the codec. The delta stage requires a 1× packet buffer for the previous-packet reference.

### 6.2 Context Model

netc uses **coarse context buckets** — not per-byte-position probability tables. A per-byte-position model would require one 256-entry frequency table per byte offset, exploding to 1500 tables for a 1500-byte packet with overfitting on small corpora.

Instead, bytes are grouped into offset ranges with one probability model per bucket:

| Context bucket | Byte offset range | Typical content |
|----------------|-------------------|-----------------|
| `CTX_HEADER`   | 0 – 15            | Protocol header, flags, type fields |
| `CTX_SUBHEADER`| 16 – 63           | Sequence numbers, session IDs, lengths |
| `CTX_BODY`     | 64 – 255          | Payload fields, state data |
| `CTX_TAIL`     | 256 – 1499        | Bulk payload, extended data |

Each bucket maintains:
- A 256-entry frequency table (tANS symbol probabilities)
- A bigram table (previous-byte → current-byte, optional, controlled by `NETC_PKT_FLAG_BIGRAM`)

Bucket boundaries are compile-time constants, not tunable at runtime in v0.1. The training algorithm assigns each corpus byte to a bucket by offset and computes per-bucket frequency tables.

### 6.3 tANS Coding

tANS (tabular ANS / FSE) decode is fully table-driven. For each symbol, decoding requires one table lookup and one state transition — no division in the decode loop:

```c
// tANS decode step (per symbol)
uint32_t slot  = state & ((1 << TABLE_LOG) - 1);   // TABLE_LOG = 12 for 4096-entry table
uint8_t  sym   = decode_table[slot].symbol;
uint8_t  nb    = decode_table[slot].nb_bits;
uint16_t next  = decode_table[slot].next_state;
state = (state >> nb) | (bitstream_peek(nb) << (32 - nb));
emit(sym);
```

The tANS state at any point is a single `uint32_t` plus the bitstream read cursor (byte offset + bit offset). Together these constitute the full checkpoint — required for UDP stateless snapshots.

Encode uses the inverse table (symbol → state transition). Both tables fit in 16 KB (4096 entries × 4 bytes) and reside in L1 cache on all modern CPUs.

### 6.4 Passthrough Threshold

If `compressed_size ≥ original_size` after the full pipeline, the packet header is written with `NETC_ALG_PASSTHRU` and the original uncompressed bytes are emitted. The caller is guaranteed that output size ≤ input size + `NETC_MAX_OVERHEAD` (8 bytes header). See AD-006.

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

### 7.3 Dictionary Format (v4)

The dictionary blob is a serialized binary format with CRC32 integrity:

```
[0..7]        Header: magic (4B 'NETC'), version (1B=4), model_id (1B),
              ctx_count (1B), flags (1B: bit 0 = DICT_FLAG_LZP)
[8..8199]     Unigram frequency tables: 16 buckets × 256 × uint16 LE
[8200..40967] Bigram frequency tables: 16 × 4 × 256 × uint16 LE
[if LZP flag set]:
  [40968..40971]               lzp_table_size (uint32 LE)
  [40972..40972+size*2-1]      LZP entries: [value:u8, valid:u8] × size
[last 4]      CRC32 checksum of all preceding data
```

**Version history**:
- v3: Unigram + bigram tables, no LZP
- v4: Adds optional LZP hash table (131072 entries x 2 bytes = 256 KB)

v3 dictionaries load successfully in v4-capable code (LZP table = NULL, XOR filter skipped).

### 7.4 Dictionary Constraints

- Maximum dictionary size: ~300 KB (v4 with LZP)
- Minimum dictionary size: ~8 KB (unigram tables only)
- Dictionary is immutable after training (read-only during compression)
- Multiple dictionaries can be active simultaneously (per-connection)
- LZP training uses Boyer-Moore majority vote with verification pass (40% confidence threshold)

---

## 8. Stream Format

netc defines two wire layouts depending on whether the caller uses stateful or stateless mode. The caller is responsible for choosing the appropriate layout and for transmitting the resulting bytes via whatever transport or buffer mechanism they use. netc has no knowledge of sockets, connections, or framing.

### 8.1 Stateful Stream Header

When the caller uses `netc_compress` (stateful mode) over an ordered, reliable channel, the channel SHOULD begin with a stream header that establishes the shared compression context parameters:

```
┌──────────┬──────────────────────────────────────────────┐
│  Stream  │ [Header][Payload0][Payload1]...[PayloadN]     │
│  Header  │                                               │
│  (16B)   │                                               │
└──────────┴──────────────────────────────────────────────┘

Stream Header:
  [0..3]  magic = 0x4E455443
  [4..5]  version
  [6..7]  flags
  [8..11] dict_id (reference to negotiated dictionary blob)
  [12]    model_id (active dictionary/model version for this stream)
  [13..15] reserved
```

This header is **optional and transport-agnostic** — the caller may negotiate `dict_id` and `model_id` out-of-band (e.g., via a handshake message, config file, or application-level protocol) and omit this header entirely. netc does not require or enforce this header format.

### 8.2 Stateless Payload

When the caller uses `netc_compress_stateless`, each compressed buffer is fully self-contained. The 8-byte packet header (§9.1) carries all necessary decoding information. No stream header is required.

```
┌──────────────────────────────────────────────────────────┐
│ [8B netc header][compressed payload]                      │
└──────────────────────────────────────────────────────────┘
```

This layout is suitable for any channel where payloads may arrive out of order, be lost, or be processed independently — regardless of whether the underlying transport is UDP, QUIC, a ring buffer, shared memory, or anything else.

### 8.3 Passthrough Mode

If compressed size ≥ original size, netc emits the original uncompressed payload with `NETC_PKT_FLAG_PASSTHRU` set. This guarantees the netc output is always ≤ input size + 8 bytes (header overhead).

---

## 9. Packet Format

### 9.1 Legacy Packet Layout (8 bytes)

The default wire format uses a fixed 8-byte header:

```
Offset  Size  Field
──────  ────  ─────────────────────────────────────────────
0       2     original_size   (uint16, max 65535)
2       2     compressed_size (uint16)
4       1     flags           (NETC_PKT_FLAG_*)
5       1     algorithm       (NETC_ALG_TANS | NETC_ALG_RANS | NETC_ALG_PASSTHRU)
6       1     model_id        (uint8, identifies dictionary + context model version)
7       1     context_seq     (uint8, rolling sequence counter for delta, UDP mode)
8       N     compressed_payload
```

**Total header size**: 8 bytes. `NETC_MAX_OVERHEAD` = 8.

### 9.1a Compact Packet Layout (2-4 bytes)

When `NETC_CFG_FLAG_COMPACT_HDR` is set on both compressor and decompressor contexts, a variable-length compact header is used instead. This eliminates redundant fields that can be derived from context state:

| Eliminated field | Recovery method |
|------------------|----------------|
| `compressed_size` | `= src_size - header_size` (caller provides `src_size`) |
| `model_id` | From `ctx->dict->model_id` (stateful) or caller's dict (stateless) |
| `context_seq` | Tracked internally by both compressor and decompressor |

```
Compact Header:

Byte 0: PACKET_TYPE (uint8)
  Structured encoding of (flags, algorithm) as a single lookup index.
  0x00-0x0F: Non-bucketed types (passthru, PCTX, MREG, LZ77X, etc.)
  0x10-0x8F: Bucketed types (TANS+bucket, LZP+bucket, with delta/bigram/x2 variants)
  0xFF: Reserved (invalid)

Byte 1: [E][SSSSSSS]
  E=0: original_size = SSSSSSS (0-127). Total header = 2 bytes.
  E=1: bytes 2-3 contain original_size as uint16 LE. Total header = 4 bytes.

Bytes 2-3 (only when E=1): original_size (uint16 LE, range 128-65535)
```

**Result**: 2-byte header for packets <= 127B, 4-byte for 128-65535B.

**ANS state compaction**: In compact mode, the tANS initial state is encoded as `uint16` (2 bytes) instead of `uint32` (4 bytes). The ANS state range [4096, 8192) requires only 13 bits, fitting in a `uint16`. This saves an additional 2 bytes per packet for single-region tANS, and 4 bytes for dual-interleaved (X2) tANS.

**Impact on overhead**:
- Legacy: 8B header + 4B ANS state = 12B minimum overhead
- Compact (packet <= 127B): 2B header + 2B ANS state = 4B minimum overhead
- Compact (packet >= 128B): 4B header + 2B ANS state = 6B minimum overhead

### 9.2 model_id — Dictionary and Model Versioning

`model_id` is an 8-bit opaque identifier assigned by the server at dictionary training time. It uniquely identifies the combination of dictionary content and context model parameters (bucket boundaries, compression level). The decompressor uses `model_id` to select the correct dictionary when multiple are loaded.

**Rolling upgrade semantics**: During a dictionary update, the server MUST accept packets from at least two consecutive `model_id` values simultaneously (the outgoing and incoming dictionary). The transition window is application-defined but SHOULD be ≥ 1 second of client heartbeat interval.

```
Server upgrade sequence:
  1. Train new dictionary → assign model_id = N+1
  2. Broadcast model_id = N+1 to all clients (out-of-band, e.g., connect message)
  3. Accept model_id = N (old) AND model_id = N+1 (new) during transition
  4. After transition window: stop accepting model_id = N
  5. Remove old dictionary from memory
```

`model_id = 0` is reserved and means "no model / passthrough only". `model_id = 255` is reserved for future use.

### 9.3 Algorithm Values

```c
#define NETC_ALG_TANS      0x01  // tANS (FSE) — primary codec, v0.1+
#define NETC_ALG_RANS      0x02  // rANS — planned for v0.2 (opt-in)
#define NETC_ALG_TANS_PCTX 0x03  // Per-position context-adaptive tANS
#define NETC_ALG_LZP       0x04  // LZP XOR pre-filter + tANS
#define NETC_ALG_LZ77X     0x05  // Cross-packet LZ77 (ring buffer)
#define NETC_ALG_PASSTHRU  0xFF  // Uncompressed passthrough
```

The upper 4 bits of the `algorithm` byte encode the table bucket index (0-15) for single-region tANS. For example, `NETC_ALG_TANS | (bucket << 4)` selects tANS with a specific frequency table bucket.

The `algorithm` field allows decoders to support multiple algorithm variants without a breaking wire format change.

### 9.4 Flags

#### Packet header flags (`NETC_PKT_FLAG_*`)

```c
#define NETC_PKT_FLAG_DELTA      0x01  // Delta-encoded from previous packet
#define NETC_PKT_FLAG_BIGRAM     0x02  // Bigram context model active
#define NETC_PKT_FLAG_PASSTHRU   0x04  // Uncompressed passthrough (see AD-006)
#define NETC_PKT_FLAG_DICT_ID    0x08  // Dictionary model_id verified
#define NETC_PKT_FLAG_LZ77       0x10  // LZ77 within-packet compression
#define NETC_PKT_FLAG_MREG       0x20  // Multi-region tANS (multiple buckets)
#define NETC_PKT_FLAG_X2         0x40  // Dual-interleaved tANS streams
#define NETC_PKT_FLAG_RLE        0x80  // RLE pre-pass applied
```

#### Configuration flags (`NETC_CFG_FLAG_*`)

```c
#define NETC_CFG_FLAG_STATEFUL    0x01  // Ordered sequential payloads
#define NETC_CFG_FLAG_STATELESS   0x02  // Independent payloads
#define NETC_CFG_FLAG_DELTA       0x04  // Enable inter-packet delta prediction
#define NETC_CFG_FLAG_BIGRAM      0x08  // Enable bigram context model
#define NETC_CFG_FLAG_STATS       0x10  // Enable statistics collection
#define NETC_CFG_FLAG_COMPACT_HDR 0x20  // Use compact 2-4B packet header (see §9.1a)
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
    size_t   ring_buffer_size;  // History ring buffer for stateful mode (default: 64KB, 0=disable)
    uint8_t  compression_level; // 0=fastest, 9=best ratio (default: 5)
    uint8_t  simd_level;        // 0=auto, 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON
    size_t   arena_size;        // Pre-allocated memory arena (0=use default)
} netc_cfg_t;

// Stateful: context accumulates history across sequential netc_compress calls.
// Use when payloads arrive in order with no loss (reliable ordered channel).
// Compatible with: TCP, QUIC streams, ordered IPC, ring buffers, any reliable medium.
#define NETC_CFG_FLAG_STATEFUL    0x01

// Stateless: each netc_compress_stateless call is fully independent.
// Use when payloads may arrive out of order, be lost, or be processed independently.
// ring_buffer_size is ignored when this flag is set.
// Compatible with: UDP datagrams, QUIC unreliable datagrams, shared memory, any medium.
#define NETC_CFG_FLAG_STATELESS   0x02

#define NETC_CFG_FLAG_DELTA       0x04  // Enable inter-payload delta prediction
#define NETC_CFG_FLAG_BIGRAM      0x08  // Enable bigram context model
#define NETC_CFG_FLAG_STATS       0x10  // Collect compression statistics
```

**Transport agnosticism**: `NETC_CFG_FLAG_STATEFUL` and `NETC_CFG_FLAG_STATELESS` describe the **calling pattern**, not the transport protocol. A caller using TCP but processing each payload independently SHOULD use `NETC_CFG_FLAG_STATELESS`. A caller using a custom reliable ordered ring buffer SHOULD use `NETC_CFG_FLAG_STATEFUL`. The choice belongs entirely to the caller.

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
    NETC_ERR_VERSION       = -8,  // Dictionary format version or model_id mismatch
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
