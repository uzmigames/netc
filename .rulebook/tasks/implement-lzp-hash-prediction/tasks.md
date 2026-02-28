## 1. Dictionary: LZP Hash Table Training
- [ ] 1.1 Define LZP constants in `netc_tans.h` or new `netc_lzp.h`
  - `NETC_LZP_ORDER` = 3 (context bytes)
  - `NETC_LZP_HT_BITS` = 17 (131072 entries, matches Oodle htbits=17)
  - `NETC_LZP_HT_SIZE` = (1 << NETC_LZP_HT_BITS)
  - LZP hash function: FNV-1a on ORDER bytes, masked to HT_SIZE-1
- [ ] 1.2 Define `netc_lzp_entry_t` struct: `{ uint8_t value; uint8_t count; }`
  - `value`: predicted byte for this context
  - `count`: training frequency (capped at 255, for majority vote)
- [ ] 1.3 Add `netc_lzp_entry_t *lzp_table` pointer to `netc_dict_t`
  - NULL when no LZP model is present (v3 compat)
  - Allocated as separate block (256KB), freed in `netc_dict_free()`
- [ ] 1.4 Implement LZP training in `netc_dict_train()`
  - Allocate temporary counter table: `lzp_counter[HT_SIZE][256]` (uint16)
  - For each training packet, for each byte at pos >= ORDER:
    hash context[pos-ORDER..pos-1] → slot
    increment lzp_counter[slot][byte_value]
  - After all packets: for each slot, find argmax → store in lzp_table[slot]
  - Set valid flag only if max_count > threshold (e.g., max_count >= 3)
  - Note: counter table is ~32MB, allocated temporarily during training only
- [ ] 1.5 Optimize LZP training memory: use streaming majority vote (Boyer-Moore)
  - Instead of full 256-way counters, use Boyer-Moore majority element algorithm
  - Per slot: `{ candidate_byte, vote_count }` — 3 bytes per slot × 131072 = 384KB
  - Much more cache-friendly than 32MB counter array
  - Trade-off: only finds strict majority (>50%), not plurality — acceptable for LZP

## 2. Dictionary: Serialization v4
- [ ] 2.1 Bump `NETC_DICT_VERSION` from 3 to 4 in `netc_internal.h`
- [ ] 2.2 Add `flags` field to dict blob header byte 7 (currently `_pad`)
  - Bit 0: `DICT_FLAG_LZP` — LZP table is present in blob
  - Bits 1-7: reserved
- [ ] 2.3 Update `netc_dict_save()` to serialize LZP table after bigram tables
  - Write lzp_table_size (uint32 LE) — 0 if no LZP model
  - Write lzp_table entries: [value][count] × lzp_table_size
  - Update checksum calculation to include LZP data
- [ ] 2.4 Update `netc_dict_load()` to deserialize LZP table
  - If version >= 4 and DICT_FLAG_LZP is set: read lzp_table_size, allocate, load
  - If version == 3 or no LZP flag: lzp_table = NULL (backward compat)
  - Validate lzp_table_size <= NETC_LZP_HT_SIZE
- [ ] 2.5 Update `netc_dict_free()` to free lzp_table
- [ ] 2.6 Update `DICT_BLOB_SIZE` calculation to be variable
  - Base size (v3): 40972 bytes
  - With LZP: base + 4 + (lzp_table_size × 2) bytes
  - Add `netc_dict_blob_size()` helper function

## 3. Compression: LZP Prediction Pre-Pass
- [ ] 3.1 Add `NETC_ALG_LZP` algorithm constant to `netc.h`
  - Value: 0x04 (after TANS=0x01, PASSTHRU=0x02, LZ77X/TANS_PCTX)
- [ ] 3.2 Implement `lzp_predict()` in `netc_compress.c`
  - Input: src bytes, src_size, lzp_table
  - Output: flag_bits (packed bitstream) + literals (byte array) + n_literals
  - For pos < ORDER: emit literal (flag=0)
  - For pos >= ORDER: hash context → lookup → compare → flag 0/1
  - Returns total encoded size: ceil(src_size/8) + n_literals
  - If encoded_size >= src_size: return -1 (LZP not beneficial)
- [ ] 3.3 Implement LZP wire format
  - `[2B n_literals LE][flag_bytes][literal_bytes]`
  - flag_bytes = ceil(src_size / 8) packed bits, MSB first
  - literal_bytes = n_literals raw bytes in order
- [ ] 3.4 Add LZP competition to `netc_compress()` main path
  - After tANS encoding succeeds, also try LZP:
    ```
    if dict->lzp_table != NULL:
      lzp_size = lzp_predict(src, src_size, dict->lzp_table, arena)
      if lzp_size < compressed_payload AND lzp_size < src_size:
        emit NETC_ALG_LZP packet
    ```
  - LZP competes with: tANS, LZ77X, passthrough — smallest wins
- [ ] 3.5 Optional: Hybrid LZP+tANS (stretch goal)
  - Feed LZP output (flags+literals) to tANS instead of emitting raw
  - New algorithm: NETC_ALG_LZP_TANS
  - Only beneficial if flag_bits have enough skew (>90% matches)
  - Benchmark to decide if worth the complexity

## 4. Decompression: LZP Inverse
- [ ] 4.1 Add `NETC_ALG_LZP` case to `netc_decompress()` switch
- [ ] 4.2 Implement `lzp_reconstruct()` in `netc_decompress.c`
  - Read n_literals from first 2 bytes
  - Read flag_bits: for each position in output:
    if flag=1 (match): hash output[pos-ORDER..pos-1] → output[pos] = predicted
    if flag=0 (miss): output[pos] = next_literal()
  - Requires dict->lzp_table for prediction (same as encoder)
- [ ] 4.3 Handle hybrid LZP+tANS decompression (if 3.5 implemented)
  - First tANS decode → get flags+literals → then lzp_reconstruct

## 5. Testing
- [ ] 5.1 Test LZP hash table training with known data
  - Train with packets containing repeated patterns
  - Verify hash entries contain correct predictions
  - Verify majority vote resolves collisions correctly
- [ ] 5.2 Test LZP predict + reconstruct round-trip
  - Various packet sizes: 8, 32, 64, 128, 256, 1024, 65535
  - Verify byte-exact reconstruction
- [ ] 5.3 Test dict v4 serialization round-trip
  - Save dict with LZP → load → verify lzp_table matches
  - Load v3 dict → verify lzp_table is NULL (backward compat)
  - Load v4 dict without LZP flag → verify lzp_table is NULL
- [ ] 5.4 Test LZP compression ratio improvement
  - Synthetic data with predictable patterns → verify ratio < tANS-only
  - Random data → verify LZP falls back to tANS (no regression)
- [ ] 5.5 Test LZP with delta pre-pass
  - Delta + LZP + tANS full pipeline round-trip
- [ ] 5.6 Test edge cases
  - Packet smaller than LZP_ORDER (should skip LZP)
  - All bytes match prediction (best case)
  - No bytes match prediction (worst case, should fall back)
  - Hash collision: different contexts map to same slot
- [ ] 5.7 Achieve 95%+ coverage for all new LZP code paths

## 6. Benchmarking
- [ ] 6.1 Run WL-001 through WL-008 with LZP-enabled dict
- [ ] 6.2 Compare netc+LZP ratio against Oodle on same workloads
- [ ] 6.3 Measure LZP training time (should be < 2x current training)
- [ ] 6.4 Measure LZP encode/decode throughput (target: > 500 MB/s)
- [ ] 6.5 Profile cache performance of LZP hash table lookup

## 7. Documentation
- [ ] 7.1 Update RFC-001 Section 6 with LZP algorithm description
- [ ] 7.2 Update RFC-001 Section 7 with dict v4 format
- [ ] 7.3 Update API docs with LZP-related configuration
- [ ] 7.4 Update CHANGELOG with LZP entry
