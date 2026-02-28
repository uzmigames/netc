## 1. Compact Header Implementation
- [x] 1.1 Define compact header types and constants in `netc_internal.h`
  - `netc_pkt_type_entry_t` struct with flags + algorithm
  - 256-entry `netc_pkt_type_table[]` decode table (0x00-0xAF active, 0xB0-0xFE reserved, 0xFF sentinel)
  - `netc_compact_type_encode()` inline encoder
  - `netc_hdr_write_compact()` / `netc_hdr_read_compact()` for 2-4 byte headers
- [x] 1.2 Add `NETC_CFG_FLAG_COMPACT_HDR` (0x20U) to public API in `netc.h`
  - Legacy 8B header remains default
  - Compact: 2B for packets ≤ 127B, 4B for 128-65535B
- [x] 1.3 Modify `netc_compress()` to emit compact header when flag is set
  - Packet type byte encodes (flags, algorithm) as single byte
  - Size varint: E=0 → 7-bit size (2B total), E=1 → 16-bit extended (4B total)
  - Eliminated compressed_size, model_id, context_seq from wire (derived from context)
- [x] 1.4 Modify `netc_decompress()` to parse compact header when flag is set
  - Table lookup: pkt_type → (flags, algorithm)
  - compressed_size = src_size - header_len
  - model_id and context_seq from ctx state
- [x] 1.5 Update `emit_passthrough()` for compact header mode
- [x] 1.6 ANS state compaction: 2B (uint16) instead of 4B in compact mode
  - ANS state range [4096, 8192) fits in 13 bits

## 2. Packet Type Table Coverage
- [x] 2.1 Passthrough variants (0x00-0x03): plain, LZ77, LZ77+DELTA, RLE
- [x] 2.2 PCTX variants (0x04-0x07): PCTX, PCTX+DELTA, PCTX+LZP, PCTX+LZP+DELTA
- [x] 2.3 MREG variants (0x08-0x0D): MREG, MREG+DELTA, MREG+X2, MREG+X2+DELTA, MREG+BIGRAM, MREG+BIGRAM+DELTA
- [x] 2.4 LZ77X (0x0E)
- [x] 2.5 TANS + bucket (0x10-0x6F): 16 buckets × 6 flag combos (plain/DELTA/BIGRAM/BIGRAM+DELTA/X2/X2+DELTA)
- [x] 2.6 LZP + bucket (0x70-0x8F): 16 buckets × 2 (plain/DELTA)
- [x] 2.7 LZP + BIGRAM + bucket (0x90-0xAF): 16 buckets × 2 (BIGRAM/BIGRAM+DELTA)

## 3. Testing
- [x] 3.1 Test compact header round-trip for all packet type ranges
- [x] 3.2 Test compact header with every flag combination (encode/decode table consistency 0x00-0xAF)
- [x] 3.3 Test size varint boundaries: 0, 1, 127, 128, 255, 32767, 65535
- [x] 3.4 Test passthrough mode — 2-byte compact header
- [x] 3.5 Test error handling: truncated 1-byte, truncated 3-byte (need 4), invalid 0xFF, reserved 0x0F
- [x] 3.6 Test backward compat: legacy 8B header still works when flag is not set
- [x] 3.7 Test compact vs legacy saves bytes (at least 4B on 64B packets)
- [x] 3.8 Test delta roundtrip in compact mode (multi-packet stream)
- [x] 3.9 Test ANS state compaction (2B vs 4B, multi-packet stream, stateless unaffected)
- [x] 3.10 Test LZP+BIGRAM compact type roundtrip (encode/decode 32 codes)
- [x] 3.11 Test LZP+BIGRAM end-to-end compress/decompress
- [x] 3.12 Test LZP+BIGRAM+DELTA end-to-end compress/decompress (10-packet stream)

## 4. Benchmarking
- [x] 4.1 Add `--compact-hdr` flag to bench harness
- [x] 4.2 All 8 workloads pass in both compact and legacy modes
- [x] 4.3 Measured ratios (compact): WL-001=0.765, WL-002=0.591, WL-003=0.349
- [x] 4.4 WL-003 compact (0.349) matches OodleNetwork (0.35)

## 5. Documentation
- [x] 5.1 README updated with compact header info, overhead breakdown table, algorithm pipeline
- [x] 5.2 CHANGELOG updated with compact header, ANS compaction, LZP+BIGRAM types
- [x] 5.3 Gap analysis table in README (netc compact vs Oodle per workload)
