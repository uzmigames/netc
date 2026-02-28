# Proposal: improve-compression-ratio-phase1

## Why
netc compression ratio lags OodleNetwork UDP by 3-11% across workloads, with the largest gap at 128B (0.603 vs 0.544 = 10.8%). The gap stems from three root causes: (1) order-1-only context models, (2) conservative Laplace frequency normalization, and (3) uniform minimum-frequency floor wasting bits on rare symbols. This task implements two high-impact improvements targeting 2-5% ratio gain.

## What Changes

### Phase 1A — Order-2 Context (Trigram) tANS Encoding
- Add optional order-2 bigram tables: `trigram_tables[bucket][class][256]` where `class = hash(prev0, prev1) % N_CLASSES`
- Train order-2 class mapping during `netc_dict_train()` using 2-byte context clustering
- Encoder tries order-2 tANS after order-1; picks smaller output
- New compact packet types for TRIGRAM variant
- Dict format v6: extends v5 with trigram class map + tables

### Phase 1B — Adaptive Frequency Normalization
- Replace uniform Laplace smoothing (add-1 to all 256 symbols) with threshold-based normalization
- Symbols with training frequency < threshold get minimum freq=1; symbols with freq=0 in training share a single escape slot
- Per-bucket adaptive threshold stored in dictionary
- Estimated 0.5-1.5% ratio improvement from tighter probability distribution

## Impact
- Affected specs: RFC-001 (compression protocol), RFC-002 (benchmark targets)
- Affected code: netc_dict.c, netc_compress.c, netc_decompress.c, netc_tans.h, netc_internal.h, include/netc.h
- Breaking change: NO (dict v5 backward compatible; v6 is additive)
- User benefit: 2-5% compression ratio improvement, closing gap with OodleNetwork
