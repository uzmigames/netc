## 1. Compact Header v2 Implementation
- [ ] 1.1 Define compact header types and constants in `netc_internal.h`
  - `netc_compact_hdr_t` struct (control byte + optional size bytes)
  - `NETC_COMPACT_HDR_MIN` (1), `NETC_COMPACT_HDR_MAX` (3) constants
  - `netc_compact_hdr_len()` inline: derive header length from byte 0
  - `netc_compact_hdr_write()` inline: serialize control + original_size
  - `netc_compact_hdr_read()` inline: deserialize, derive compressed_size from src_size
- [ ] 1.2 Add `NETC_CFG_FLAG_COMPACT_HDR` (0x20U) to public API in `netc.h`
  - Update `NETC_MAX_OVERHEAD` docs to note it is 8 for legacy, 3 for compact
  - Add `netc_compress_bound_v2()` or make `netc_compress_bound()` context-aware
- [ ] 1.3 Modify `netc_compress()` to emit compact header when flag is set
  - Use `size_mode=00` for packets 1-255, `size_mode=01` for 256-65535
  - Use `size_mode=10` for passthrough (header = 1 byte only)
  - Pack flags into control byte bits [5:0]
  - Do NOT write compressed_size, algorithm, model_id, context_seq
- [ ] 1.4 Modify `netc_decompress()` to parse compact header when flag is set
  - Derive `compressed_size = src_size - compact_header_len`
  - Reconstruct algorithm from flags (MREG/LZ77/RLE/passthrough combinations)
  - Use per-context model_id and algorithm (from ctx->dict and ctx->flags)
- [ ] 1.5 Update `emit_passthrough()` for compact header mode
- [ ] 1.6 Update stateless API paths for compact header

## 2. Headerless Raw Codec API
- [ ] 2.1 Add `netc_compress_raw()` to public API and implement
  - Same signature as `netc_compress()` plus `uint8_t *flags_out`
  - Writes compressed payload only (no header bytes)
  - Returns flags byte for caller to embed in their own framing
- [ ] 2.2 Add `netc_decompress_raw()` to public API and implement
  - Requires caller to provide `original_size` and `flags`
  - Reads compressed payload directly (no header parsing)
- [ ] 2.3 Add `NETC_CFG_FLAG_HEADERLESS` (0x40U) config flag

## 3. Testing
- [ ] 3.1 Test compact header round-trip for all size_mode values (00, 01, 10)
- [ ] 3.2 Test compact header with every flag combination (delta, bigram, RLE, LZ77, X2, MREG)
- [ ] 3.3 Test compact header boundary: packet sizes 1, 127, 128, 255, 256, 65535
- [ ] 3.4 Test passthrough mode (size_mode=10) — 1-byte header
- [ ] 3.5 Test headerless raw API round-trip
- [ ] 3.6 Test error handling: truncated compact header, invalid size_mode=11
- [ ] 3.7 Test backward compat: legacy 8B header still works when flag is not set
- [ ] 3.8 Verify compression ratio improvement on 32B, 64B, 128B synthetic packets
- [ ] 3.9 Achieve 95%+ coverage for all new code paths

## 4. Benchmarking
- [ ] 4.1 Add compact header mode to bench harness (bench_netc.c)
- [ ] 4.2 Measure ratio improvement vs legacy header on WL-001 through WL-008
- [ ] 4.3 Compare compact-header netc ratio against Oodle on same workloads

## 5. Documentation
- [ ] 5.1 Update RFC-001 Section 9 with Compact Header v2 specification
- [ ] 5.2 Update API reference docs with new functions and flags
- [ ] 5.3 Update CHANGELOG with header optimization entry
