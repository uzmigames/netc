# Proposal: compact-packet-header

## Why
The current 8-byte packet header represents 12.5-25% overhead on 32-64 byte game packets,
which is the dominant factor preventing netc from matching Oodle Network's 0.55 compression
ratio on small packets. Analysis shows that 5 of the 8 header bytes carry per-connection
information that is redundant when transmitted per-packet: `compressed_size` (2B, derivable
from transport frame length), `algorithm` (1B, per-connection), `model_id` (1B, per-connection),
and `context_seq` (1B, implicit in stateful mode). Oodle Network achieves 0B overhead by being
a headerless codec. A compact 2-byte header closes most of the gap while maintaining netc's
self-describing packet property.

## What Changes

### Phase A: Compact Header v2 (2-3 byte header)
1. New wire format: 1-byte control byte encodes flags + size_mode, followed by 0-2 bytes
   for original_size (VarInt-style).
2. `compressed_size` eliminated (derived from transport frame).
3. `algorithm`, `model_id`, `context_seq` moved to per-connection context state.
4. `NETC_CFG_FLAG_COMPACT_HDR` opt-in flag added to netc_cfg_t.
5. `netc_compress_bound()` becomes context-aware (returns src_size + 2 or src_size + 3).

### Phase B: Headerless Raw Codec API
1. `netc_compress_raw()` / `netc_decompress_raw()` — zero-overhead codec access.
2. Caller provides `original_size` and receives `flags` out-of-band.
3. Matches Oodle's architectural model for game engines with existing framing.

### Wire Format (Compact Header v2)
```
Byte 0: Control byte
  Bits [7:6] = size_mode:
    00 = original_size follows as 1 byte  (packets 1-255)
    01 = original_size follows as 2 bytes LE (packets 256-65535)
    10 = passthrough (no size needed)
    11 = extended header (reserved)
  Bit [5] = DELTA
  Bit [4] = BIGRAM
  Bit [3] = RLE
  Bit [2] = LZ77
  Bit [1] = X2 (dual-interleaved)
  Bit [0] = MREG

Payload follows immediately after the header.
```

## Impact
- Affected specs: RFC-001 Section 9 (Packet Format), include/netc.h
- Affected code: netc_internal.h (header types), netc_compress.c, netc_decompress.c
- Breaking change: NO (opt-in via NETC_CFG_FLAG_COMPACT_HDR, legacy 8B header is default)
- User benefit: ~14% absolute ratio improvement on 64B packets (0.672 -> 0.578).
  Headerless mode matches Oodle at 0.547 ratio on same data.
