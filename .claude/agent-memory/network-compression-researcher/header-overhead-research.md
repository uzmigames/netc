# Header Overhead Research (2026-02-28)

## Problem
8B header on 32-64B game packets = 12.5-25% overhead, destroying compression ratio.

## Oodle Network Approach
- **0 bytes per-packet overhead** (headerless codec)
- rawLen passed out-of-band via API parameters
- Dictionary/model negotiated at connection setup
- Confirmed by bench_oodle.c: "Oodle is headerless -- rawLen must be transmitted out-of-band"

## Field-by-field Analysis of Current 8B Header
| Field | Size | Per-Packet? | Recommendation |
|-------|------|-------------|----------------|
| original_size | 2B | Yes (variable pkts) | VarInt: 1B for <=255 |
| compressed_size | 2B | NO | Derive from transport frame len |
| flags | 1B | YES | Keep, merge into control byte |
| algorithm | 1B | NO | Per-connection state |
| model_id | 1B | NO | Per-connection state |
| context_seq | 1B | Conditional | Stateful: implicit. Stateless: transport seq |

## Compact Header v2 Design
```
Byte 0: [size_mode:2][delta:1][bigram:1][rle:1][lz77:1][x2:1][mreg:1]
Byte 1: original_size (1B) when size_mode=00
Bytes 1-2: original_size (2B LE) when size_mode=01
No extra bytes when size_mode=10 (passthrough)
```

## Impact on 64B Packet (35B compressed payload)
- Current 8B header: 43B total = 0.672 ratio
- Compact 2B header: 37B total = 0.578 ratio (14% improvement)
- Headerless 0B: 35B total = 0.547 ratio (matches Oodle)

## Related Techniques
- ROHC (RFC 3095): per-flow context, differential encoding, 1-3B headers
- QUIC VarInt: 1-8B variable-length integers
- ENet: 2B header with flags+size merged
- SteamNetworkingSockets: 1B compression control prefix
- Zstd FSE frames: VarInt size + raw bitstream, no per-block algorithm/model
