# Proposal: implement-adaptive-cross-packet-learning

## Why

netc beats OodleNetwork UDP on all 5 workloads (OODLE-01 PASSED), but Oodle TCP
still wins on WL-004 32B (0.572 vs 0.658) and WL-005 512B (0.415 vs 0.437) due to
**stateful cross-packet learning** — Oodle TCP continuously updates its entropy model
using every packet it processes, accumulating better predictions over time.

netc's current architecture is **static after training**: tANS frequency tables, LZP
hash predictions, and bigram class maps are frozen at dictionary creation and never
updated. The only dynamic state is `prev_pkt` (single-packet delta) and the LZ77
ring buffer. This leaves significant ratio headroom on the table.

Adding adaptive learning would close the gap to Oodle TCP and potentially surpass it,
since netc already has stronger base models (PCTX per-position tables, bigram-PCTX).

## What Changes

### Phase 1 — Adaptive tANS Frequency Tables (main win)

Per-context mutable frequency counters that accumulate byte statistics across packets.
Every N packets, rebuild tANS tables from the accumulated frequencies blended with the
dictionary baseline. The encoder and decoder both see the same bytes, so both can update
counters deterministically — no sync protocol needed.

Key design: **encoder and decoder update identically after each packet.** Both see the
decompressed bytes (encoder has the original, decoder has the reconstructed). Both run
the same `freq_update()` after processing. Tables converge automatically.

- Add `uint32_t freq_accum[NETC_CTX_COUNT][256]` to `netc_ctx_t` (~16 KB per context)
- Add `uint32_t freq_total[NETC_CTX_COUNT]` — total byte count per bucket
- After each packet (encode or decode), increment counters for bytes seen
- Blend with dictionary baseline: `final_freq[s] = alpha * dict_freq[s] + (1-alpha) * accum_freq[s]`
- Rebuild tANS tables every `NETC_ADAPTIVE_INTERVAL` packets (e.g., 64 or 128)
- No new packet header bits needed — deterministic sync

### Phase 2 — Adaptive LZP Hash Updates

Update LZP hash table entries on prediction misses. Currently the LZP table is
read-only (shared dictionary). For adaptive mode, each context gets a mutable copy
of the LZP table that evolves per-connection.

- Clone LZP table into context on first packet (~262 KB per context, opt-in)
- On LZP miss: update `lzp_table[h].value = actual_byte`
- Both encoder and decoder do the same update → stays in sync
- Flag: `NETC_CFG_FLAG_ADAPTIVE` enables this behavior

### Phase 3 — Multi-Packet Delta (order-2)

Extend delta prediction to use 2 previous packets instead of 1. For many game
protocols, fields evolve with constant velocity (position += velocity * dt), so
the second derivative (acceleration) is often near-zero.

- Add `prev2_pkt` buffer to context (+64 KB)
- Order-2 predictor: `predicted[i] = 2*prev[i] - prev2[i]` (linear extrapolation)
- Fall back to order-1 when `prev2_pkt_size != prev_pkt_size` (size mismatch)
- Compare order-1 vs order-2 residuals; use smaller

## Impact

- Affected specs: RFC-001 (new adaptive mode), RFC-002 (new ratio targets)
- Affected code: netc_internal.h (context struct), netc_compress.c, netc_decompress.c,
  netc_dict.c (table rebuild), netc_tans.c (mutable table support), netc_lzp.h
- Breaking change: NO (adaptive is opt-in via new flag; existing behavior unchanged)
- User benefit: Better compression ratio on sustained connections (game servers,
  telemetry streams). Closes gap to Oodle TCP and potentially surpasses it.

## Memory Budget

| Component | Per-context cost | Opt-in |
|-----------|:----------------:|:------:|
| Adaptive freq counters | ~16 KB | Phase 1 |
| Mutable LZP table | ~262 KB | Phase 2 |
| prev2_pkt buffer | ~64 KB | Phase 3 |
| **Total (all phases)** | **~342 KB** | Within 512 KB limit |
