# Algorithm Design Decisions

```
Date:     2026-02-26
Version:  0.1.0
Status:   DRAFT
```

## Decision Log

### AD-001: ANS over Huffman as primary codec

**Decision**: Use Asymmetric Numeral Systems (rANS) as the primary entropy coder, with Huffman as fallback for edge cases.

**Rationale**:
- ANS achieves fractional-bit precision vs. Huffman's integer-bit rounding (5–15% better ratio)
- ANS decode speed is comparable to Huffman decode with lookup tables
- Zstd uses FSE (Finite State Entropy = tANS), validating the approach at production scale
- rANS state is a single 64-bit integer — trivial to checkpoint/restore for UDP stateless mode

**Rejected alternatives**:
- Pure Huffman: Integer bit limitation costs ratio on skewed distributions
- Arithmetic coding: Slower than ANS, patent concerns historically (now expired but reputation remains)
- LZ77-based: Works well for long matches, but game packets are too small for match finding to pay off

### AD-002: Delta encoding as a pre-pass, not integrated into ANS

**Decision**: Apply byte-level delta prediction before entropy coding as a separate stage.

**Rationale**:
- Separation of concerns: delta prediction logic is independent of codec
- Delta can be disabled without changing the codec
- Simpler implementation and testing
- Delta residuals are independently entropy-coded per position, matching how ANS probability tables are organized

**Tradeoff**: Requires 2× packet buffer for the delta pass. For 1500-byte packets, this is 3KB extra — acceptable.

### AD-003: Separate tANS for small packets (≤ 64 bytes)

**Decision**: Use tabular ANS (tANS/FSE) for packets ≤ 64 bytes, rANS for larger.

**Rationale**:
- rANS requires normalization (flush) every few symbols; for 8-symbol packets, flush overhead dominates
- tANS uses a fixed-size decode table (4096 entries for 12-bit table = 8KB, fits in L1)
- tANS decode is branch-free: single table lookup + state transition

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
