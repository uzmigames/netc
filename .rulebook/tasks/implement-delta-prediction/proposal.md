# Proposal: Implement Delta Prediction (Inter-Packet Correlation)

## Why

Game and simulation packets are highly correlated across time: player positions change by small deltas each tick, enum values repeat, counters increment by 1. By subtracting predicted byte values before entropy coding, the residuals have significantly lower entropy (1–2 bits/byte instead of 3–5 bits/byte), improving compression ratio by 20–40% over ANS alone. This is the key differentiator that lets netc approach Oodle Network's ratios.

## What Changes

- Implement byte-level delta encoder/decoder (src/algo/netc_delta.c)
- Implement ring buffer history for TCP stateful mode (src/core/netc_ctx.c update)
- Implement sequence-based delta for UDP stateless mode
- Implement bigram context model (position-aware probability tables)
- Wire delta pre-pass into compression pipeline before ANS encoding

## Impact

- Affected specs: delta/spec.md (new)
- Affected code: src/algo/netc_delta.c (new), src/core/netc_ctx.c (updated — ring buffer), src/core/netc_compress.c (updated — delta pre-pass), src/core/netc_decompress.c (updated — delta post-pass)
- Breaking change: NO (new flag NETC_PKT_FLAG_DELTA, backward-compatible)
- User benefit: 20–40% improvement in compression ratio for correlated packet streams (TCP game servers, market data feeds)
