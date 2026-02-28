# Proposal: optimize-compress-throughput

## Why

Compress throughput is 26-45 MB/s — 5-10x slower than OodleNetwork UDP (168-305 MB/s) and 10-40x slower than LZ4 (231-1020 MB/s). The root cause is the multi-codec competition architecture: the encoder tries 3-7 different codec paths per packet (bucket scanning, delta-vs-LZP trial, LZ77 fallback), each requiring a full re-encoding pass. For a 64B game packet, this means ~7 passes over the data where competitors do 1. Throughput is critical for game servers handling 10-100k players at 60+ tick rate.

### Current throughput (MSVC Release, compact headers)

| Workload | netc (MB/s) | Oodle UDP (MB/s) | LZ4 (MB/s) | Gap |
|----------|:-----------:|:-----------------:|:-----------:|:---:|
| WL-004 32B | 44.5 | 205.7 | 231.7 | 4.6x |
| WL-001 64B | 26.0 | 191.1 | 408.7 | 7.3x |
| WL-002 128B | 29.9 | 205.7 | 621.0 | 6.9x |
| WL-003 256B | 38.8 | 305.3 | 1001.0 | 7.9x |
| WL-005 512B | 36.9 | 168.7 | 1020.1 | 4.6x |

### Bottleneck breakdown (per 64B packet)

| Step | Passes | Impact |
|------|:------:|--------|
| Bucket scanning (try 2-5 tables) | 2-5x tANS encode | **2-3x loss** |
| Delta-vs-LZP trial re-encoding | 1x extra tANS encode | **1.5-2x loss** |
| LZ77 trial after tANS | 1x LZ77 + hash init | **1.1-1.3x loss** |
| PCTX 16-if bucket lookup per byte | N branch misses | **1.2-1.5x loss** |
| LZP hash (3 muls) per byte | N multiplies | **15-25% loss** |
| Per-byte delta branch (XOR vs SUB) | N branches | **30-50% loss** |

## What Changes

Six targeted optimizations in the compress hot path, ordered by impact. Each is independently testable and mergeable. Zero-loss optimizations first, then controlled-loss ones.

### Phase 1 — Zero ratio loss (target: 2-3x speedup)
1. **PCTX bucket lookup table**: replace 16-if ladder in `netc_ctx_bucket()` with 256-byte LUT
2. **Skip LZ77 for packets <256B**: LZ77 never wins on small game packets, eliminate hash table init + scan
3. **SIMD delta dispatch**: ensure SSE4.2/AVX2 delta encode/decode is wired through in the bench hot path
4. **Fuse LZP+tANS loop**: single pass instead of LZP filter → arena copy → tANS encode

### Phase 2 — Controlled ratio loss (<2%, target: 3-5x total speedup)
5. **Reduce bucket scan to max 2**: only try first-byte bucket + last-byte bucket, skip intermediates
6. **Skip delta-vs-LZP trial**: when delta+tANS succeeds on packets ≤512B, skip the LZP-only re-trial

### Phase 3 — Measure and document
7. Benchmark all 8 workloads after each phase, track ratio delta and throughput gain
8. Update CHANGELOG, README, and bench with throughput results

## Impact
- Affected specs: RFC-001 (no wire format change), RFC-002 (throughput targets updated)
- Affected code: `src/core/netc_compress.c`, `src/algo/netc_tans.h`, `src/algo/netc_delta.h`, `src/algo/netc_lzp.h`
- Breaking change: NO (wire format unchanged, all existing compressed packets decompress correctly)
- User benefit: 3-5x faster compress throughput (target: 100-150 MB/s), making netc viable for high-tickrate game servers without dedicated compression threads
