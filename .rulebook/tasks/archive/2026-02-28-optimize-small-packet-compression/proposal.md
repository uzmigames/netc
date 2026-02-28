# Proposal: optimize-small-packet-compression

## Why

WL-001 (64B) and WL-002 (128B) compact mode ratios (0.765, 0.591) still have a gap vs OodleNetwork (0.68, 0.52). Even removing all per-packet overhead (4-6B), the entropy coding itself produces larger output than Oodle (0.703 vs 0.68, 0.545 vs 0.52). The root causes are:

1. **4096-entry tANS table is oversized** for 64B packets — per-symbol overhead is higher with larger tables for infrequent symbols
2. **Only 6 of 16 position buckets used** for 64B packets — sparse training data per bucket degrades frequency estimates
3. **Bigram quantization too coarse** — 4 classes via `prev_byte >> 6` loses fine-grained conditional distributions
4. **No adaptive table sizing** — always uses 12-bit (4096) regardless of packet size

## What Changes

### Phase 1: Adaptive tANS Table Size (highest impact, ~1-2%)
Try smaller tANS tables (10-bit/1024 entries) for small packets (≤ 128B). Smaller tables have lower per-symbol overhead for infrequent symbols. The compressor tries both 12-bit and 10-bit tables and keeps the smaller output.

### Phase 2: Refined Bigram Quantization (moderate impact, ~0.5-1%)
Replace the 4-class `prev_byte >> 6` scheme with an 8-class trained mapping. Group previous bytes by similar conditional distributions learned from the training corpus, not by arbitrary bit shifts.

### Phase 3: Fine-Grained Bucketing for Small Packets (high impact, ~1.5-2%)
For packets ≤ 128B, use 32-way position bucketing instead of 16. Each bucket covers ~4 bytes instead of ~8-16, giving more precise per-position frequency models.

### Phase 4: Dual-Dictionary Strategy (moderate impact, ~1-1.5%)
Train separate frequency tables optimized for small packets (≤ 128B) vs large packets (≥ 256B). One-size-fits-all tables are suboptimal for both size classes.

Expected cumulative gain: 4-6% → ratio ~0.72-0.73 on WL-001 (vs current 0.765).

## Impact
- Affected specs: RFC-001 Section 6 (Compression), Section 7 (Dictionary)
- Affected code: `src/algo/netc_tans.h`, `src/algo/netc_tans.c`, `src/core/netc_dict.c`, `src/core/netc_compress.c`, `src/core/netc_internal.h`
- Breaking change: NO (new table sizes are opt-in; existing dicts continue to work)
- User benefit: 4-6% compression ratio improvement on small packets (32-128B), closing the gap to OodleNetwork
