## 1. Delta Encoder/Decoder
- [x] 1.1 Implement delta_encode(prev, curr, residual, size) — byte subtraction with wraparound
- [x] 1.2 Implement delta_decode(prev, residual, curr, size) — byte addition reconstruction
- [x] 1.3 Implement position-weighted delta (header bytes vs payload bytes different weights)
      Field-class aware: HEADER(0-15)=XOR, SUBHEADER(16-63)=SUB, BODY(64-255)=XOR, TAIL(256+)=SUB
- [x] 1.4 Write tests: delta encode/decode round-trip (test_delta.c) — 21 tests, all passing

## 2. Ring Buffer History (Stateful Mode)
- [x] 2.1 Ring buffer already in netc_ctx_t from Phase 2 (64KB, configurable)
- [x] 2.2 Added prev_pkt buffer (NETC_MAX_PACKET_SIZE bytes) to netc_ctx_t for delta predictor
- [x] 2.3 prev_pkt updated after each compress/decompress call
- [x] 2.4 Tests: ctx_reset clears delta history, size-mismatch skips delta

## 3. Stateless Delta (Stateless Mode)
- [x] 3.1 Implement context_seq=0 for stateless (no per-packet state, per RFC-001 §9.1)
- [x] 3.2 netc_decompress_stateless rejects NETC_PKT_FLAG_DELTA with NETC_ERR_CORRUPT (no history → cannot decode)
- [x] 3.3 Write tests: stateless delta rejection, LZ77 stateless round-trip, context_seq=0 assertion (4 tests)

## 4. Coarse Context Bucket Model (per RFC-001 §6.2 — AD-003)
- [x] 4.1 4-bucket context model already in Phase 2: CTX_HEADER(0–15), CTX_SUBHEADER(16–63), CTX_BODY(64–255), CTX_TAIL(256–1499)
- [x] 4.2 Each bucket has one 256-entry tANS frequency table (implemented in Phase 2)
- [ ] 4.3 Implement optional per-bucket bigram table (enabled via NETC_PKT_FLAG_BIGRAM)
- [ ] 4.4 Add bucket frequency tables to dictionary training (netc_dict_train update)
- [ ] 4.5 Write tests: coarse bucket model improves ratio on structured data vs single-table

## 5. Pipeline Integration
- [x] 5.1 Wire delta pre-pass (field-class aware, per AD-002) before tANS in netc_compress
- [x] 5.2 Wire delta post-pass after tANS in netc_decompress
- [x] 5.3 NETC_PKT_FLAG_DELTA flag already in packet header (netc.h, Phase 1)
- [x] 5.4 Verify: delta disabled for packets < NETC_DELTA_MIN_SIZE (8 bytes) — spec test passing

## 6. Validation
- [x] 6.2 Round-trip tests: multi-packet sequences (10 packets) all round-trip correctly
- [ ] 6.1 Benchmark: verify delta improves compression ratio ≥ 10% on WL-001 vs no-delta (Phase 5)
- [ ] 6.3 Verify 95%+ test coverage on delta module (Phase 5)
