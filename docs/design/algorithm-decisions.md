# Algorithm Design Decisions

```
Date:     2026-02-26
Version:  0.1.0
Status:   DRAFT
```

## Decision Log

### AD-001: tANS (FSE) as primary codec, rANS as secondary

**Decision**: Use tabular ANS (tANS/FSE) as the default entropy coder. rANS is available as an opt-in secondary path and will be evaluated by the benchmark harness before being promoted.

**Rationale**:
- tANS decode is fully table-driven: one table lookup + one state transition per symbol. No division in the decode loop.
- rANS requires 64-bit division/modulo for every symbol encode step — measurably slower on some CPUs in hot-path conditions, despite being theoretically parallelizable.
- Zstd uses FSE (= tANS) for all entropy coding at production scale. This is strong prior art.
- tANS is a single implementation covering all packet sizes — simpler codebase, simpler fuzz surface.
- rANS checkpoint is NOT just saving a u64 state. It requires saving: state word + bitstream cursor (byte offset + bit offset within byte) + any pending renormalization. This makes rANS more complex for UDP stateless snapshots than originally stated in this ADR.

**v0.2 path for rANS**: If the benchmark harness (RFC-002) shows rANS outperforms tANS on the ≥ 256-byte workloads (WL-003, WL-005), rANS will be added as an explicit codec path selected via the `algorithm` field in the packet header.

**Rejected alternatives**:
- Pure Huffman: Integer bit limitation costs ratio on skewed distributions
- Arithmetic coding: Slower than ANS, patent concerns historically (now expired but reputation remains)
- LZ77-based: Works well for long matches, but game packets are too small for match finding to pay off

### AD-002: Delta encoding as a pre-pass — field-class aware, not byte-a-byte blind

**Decision**: Apply delta prediction before entropy coding as a separate, disableable stage. The delta strategy is **field-class aware**: different prediction functions are applied depending on the byte's structural role, not uniformly across all bytes.

**Field classes and their delta strategies**:

| Field class | Examples | Delta strategy |
|-------------|----------|----------------|
| Integer counters | sequence numbers, tick IDs, entity IDs | Byte subtraction from previous packet (wrapping) |
| Enum / flags | message type, ability flags, bitmasks | XOR with previous packet byte |
| Float components | position.x/y/z, velocity, rotation | XOR with previous packet byte (preserves mantissa delta better than subtraction) |
| Fixed strings / GUIDs | player ID, asset hash | Passthrough (high entropy, delta would increase size) |
| Repeated constants | protocol version, static headers | Passthrough or skip (entropy already near 0) |

Field classes are inferred from the training corpus (entropy + variance per byte offset), not from an explicit schema. This keeps netc schema-agnostic while still being smarter than uniform byte subtraction.

**Rationale**:
- Blind byte-a-byte subtraction on IEEE 754 floats produces high-entropy residuals (exponent bits cancel poorly). XOR preserves the mantissa delta pattern which has lower entropy.
- Separation of concerns: delta stage is independent of the entropy codec.
- Delta can be disabled per-packet (NETC_PKT_FLAG_DELTA unset) without changing the codec.

**Tradeoff**: Requires 2× packet buffer for the delta pass. For 1500-byte packets, this is 3KB extra — within the arena budget.

**What this is NOT**: This is not a schema-aware codec (no per-field protobuf/flatbuffer introspection). Field class inference is statistical, not structural. Schema-aware transforms are a v0.2+ feature.

### AD-003: Single tANS codec for all packet sizes; threshold determined by benchmark

**Decision**: Use tANS (FSE) as the single entropy coder for all packet sizes. The "tANS for small, rANS for large" split from the original draft is **deferred pending benchmark data**.

**Rationale**:
- The original threshold of "≤ 64 bytes" was a heuristic, not a measured crossover point. The real crossover (if any) depends on CPU microarchitecture, table size, flush frequency, and packet distribution — all measurable by the benchmark harness.
- Maintaining two separate codecs (tANS + rANS) doubles the implementation surface, the test/fuzz surface, and the number of decoder paths.
- tANS with a 12-bit table (4096 entries × 4 bytes = 16 KB) fits in L1 on all modern CPUs. Its branch-free decode loop is efficient at all packet sizes.
- If benchmark data (RFC-002, WL-001 through WL-008) shows a measurable throughput gain from a size-based split, the threshold will be determined empirically and added as a build-time constant, not a fixed magic number in the spec.

**What changed from the original draft**: The `≤ 64 bytes` threshold and the tANS/rANS split are removed from v0.1 scope. The packet header `algorithm` field already accommodates future algorithm variants without a breaking wire format change.

### AD-004: Static dictionary, no adaptive update during operation

**Decision**: Dictionary is trained once offline and frozen. No online adaptation during compression.

**Rationale**:
- Adaptive dictionaries require synchronization between compressor and decompressor
- For UDP, stateless operation means no shared adaptive state
- Oodle Network uses the same approach and achieves production-quality ratios
- Allows dictionary to be stored as read-only memory (shared between threads without locking)

**Tradeoff**: Dictionary must be retrained when packet distribution changes significantly. Acceptable for game servers where packet structure is stable per game title.

### AD-005: Memory arena, no malloc in hot path

**Decision**: All working memory pre-allocated at context creation using a user-configurable arena.

**Rationale**:
- malloc() can take 100–10,000 ns depending on heap state — unacceptable for sub-microsecond latency
- Arena allocation is deterministic: pointer bump, O(1)
- Allows caller to provide memory (useful for custom allocators, kernel contexts)

**Implementation**: Default arena = 2× max_packet_size (3000 bytes). Configurable via `netc_cfg_t.arena_size`.

### AD-006: Passthrough guarantee

**Decision**: If compressed size ≥ original size, emit original bytes with passthrough flag. NEVER expand payload.

**Rationale**:
- Random or already-compressed data has entropy ≈ 8 bits/byte — any compression attempt adds overhead
- Network stack has fixed MTU; expanding packets causes fragmentation
- Passthrough is a hard contract: callers can rely on output ≤ input + NETC_MAX_OVERHEAD (8 bytes header)

### AD-007: C11, no C++ or Zig

**Decision**: Implement in C11 (not C99, not C++, not Zig).

**Rationale**:
- C11 provides `_Alignas`, `_Atomic`, and `static_assert` which are useful for SIMD alignment and safety checks
- C11 is universally supported (GCC 4.9+, Clang 3.4+, MSVC 2019+)
- C is the universal FFI baseline — Rust, Go, Python, Java all call C directly
- Zig has better safety guarantees but immature ecosystem; FFI compatibility requires C ABI anyway
- C++ templates add complexity and compilation time without meaningful benefit for this use case

**Known caveat — MSVC `_Atomic`**: MSVC's C11 support for `_Atomic` is incomplete in practice (MSVC 2019 supports it partially; MSVC 2022 is better but has edge cases with compound types). See AD-008.

### AD-008: netc_platform.h — portability abstraction for atomics and alignment

**Decision**: All uses of `_Atomic`, `_Alignas`, and compiler-specific intrinsics are isolated behind a `src/util/netc_platform.h` header. No direct use of these constructs outside that header.

**Rationale**:
- MSVC C11 `_Atomic` support is incomplete. MSVC requires `<stdatomic.h>` from C11 mode (`/std:c11`) but behavior of atomic compound ops on non-trivial types differs from GCC/Clang.
- Rather than sprinkling `#ifdef _MSC_VER` throughout the codebase, a single platform header provides:
  - `NETC_ATOMIC(T)` — expands to `_Atomic T` (GCC/Clang) or `volatile T` + `InterlockedExchange` (MSVC fallback)
  - `NETC_ALIGN(N)` — expands to `_Alignas(N)` (GCC/Clang/MSVC 2022+) or `__declspec(align(N))` (MSVC fallback)
  - `NETC_LIKELY(x)` / `NETC_UNLIKELY(x)` — branch prediction hints
  - `NETC_INLINE` — `__attribute__((always_inline))` or `__forceinline`
- This keeps the core implementation clean C11 while remaining buildable on all target toolchains.

**Scope**: `netc_platform.h` is an internal header only. It is not part of the public `netc.h` API.

### AD-009: Compact packet header (8B → 2-4B)

**Decision**: Add an opt-in compact header mode (`NETC_CFG_FLAG_COMPACT_HDR`) that reduces the per-packet header from 8 bytes to 2-4 bytes by eliminating redundant fields.

**Eliminated fields**:
- `compressed_size`: derivable as `src_size - header_size` (caller provides `src_size`)
- `model_id`: known from `ctx->dict->model_id` in stateful mode
- `context_seq`: tracked internally by both compressor and decompressor

**Remaining fields** encoded compactly:
- Byte 0: `PACKET_TYPE` — structured uint8 encoding (flags + algorithm) via 144-entry lookup table
- Byte 1: `[E][SSSSSSS]` — E=0: size in 7 bits (2B total); E=1: extended uint16 LE in bytes 2-3 (4B total)

**Rationale**:
- 8B header = 12.5% overhead on 64B packets — the single largest contributor to ratio gap vs. Oodle
- Oodle has 0 bytes of per-packet overhead; compact header closes 6B of the gap
- Opt-in preserves backward compatibility; legacy 8B header remains the default
- Packet type encoding is structured arithmetic (not a flat lookup table), with const decode table for O(1) decode

**Measured impact** (WL-001 64B, compact vs legacy): 0.908 → 0.814 (6B saved).

### AD-010: ANS state compaction (4B → 2B)

**Decision**: In compact header mode, encode the tANS initial state as `uint16` (2 bytes) instead of `uint32` (4 bytes).

**Rationale**:
- tANS state range is [TABLE_SIZE, 2*TABLE_SIZE) = [4096, 8192) — only 13 bits of information
- A `uint16` holds up to 65535, far more than needed for the 13-bit range
- 2B savings per single-region tANS packet; 4B savings per X2 (dual-interleaved) packet
- Combined with compact header: 8B total savings on 64B packets (12.5% → 3.1% overhead)
- Only active in compact mode; stateless path always uses legacy 4B for compatibility

**Measured impact** (WL-001 64B, compact+ANS vs compact-only): 0.814 → 0.783 (additional 2B saved).

### AD-011: LZP XOR pre-filter (position-aware order-1 context)

**Decision**: Before tANS encoding, XOR each byte with a position-aware order-1 context prediction. Correctly predicted bytes become `0x00`, concentrating the distribution for better entropy coding.

**LZP model**:
- Context: `hash(previous_byte, byte_position_in_packet)` → predicted byte
- Hash table: 131072 entries (2^17), 256 KB, FNV-1a hash
- Training: Boyer-Moore majority vote with verification pass (40% confidence threshold)
- tANS tables retrained on LZP-filtered data during dictionary training

**Why XOR filter over flag-bit LZP**: Analysis showed that pure flag-bit LZP (predict/reconstruct with explicit flag bitstream) doesn't outperform the XOR approach for the target workloads. The XOR filter:
- Preserves the data size (no flag bitstream overhead)
- Feeds directly into tANS multi-region encoding without format change
- Benefits from tANS being trained on the filtered distribution

**Interaction with delta**: LZP XOR is applied when delta was NOT applied. Composing LZP XOR + XOR delta is mathematically equivalent to plain XOR delta (predictions cancel: `(curr^pred)^(prev^pred) = curr^prev`). This is by design — the two stages are mutually exclusive, not composable.

**Measured impact**: LZP improves first-packet ratio where delta is unavailable. For subsequent packets with delta enabled, delta takes priority as it exploits temporal correlation directly.
