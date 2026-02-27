## 1. Delta Encoder/Decoder
- [ ] 1.1 Implement delta_encode(prev, curr, residual, size) — byte subtraction with wraparound
- [ ] 1.2 Implement delta_decode(prev, residual, curr, size) — byte addition reconstruction
- [ ] 1.3 Implement position-weighted delta (header bytes vs payload bytes different weights)
- [ ] 1.4 Write tests: delta encode/decode round-trip (test_delta.c)

## 2. Ring Buffer History (Stateful Mode)
- [ ] 2.1 Add ring buffer to netc_ctx_t (configurable size, default 64KB)
- [ ] 2.2 Implement ring buffer write on each compressed packet
- [ ] 2.3 Implement ring buffer read for delta prediction lookup
- [ ] 2.4 Write tests: ring buffer correctness and wrap-around

## 3. Stateless Delta (Stateless Mode)
- [ ] 3.1 Implement context_seq tracking for stateless delta (per RFC-001 §9.1)
- [ ] 3.2 Implement out-of-order detection (skip delta if context_seq gap > 1)
- [ ] 3.3 Write tests: stateless delta with context_seq numbers

## 4. Coarse Context Bucket Model (per RFC-001 §6.2 — AD-003)
- [ ] 4.1 Implement 4-bucket context model: CTX_HEADER(0–15), CTX_SUBHEADER(16–63), CTX_BODY(64–255), CTX_TAIL(256–1499)
- [ ] 4.2 Each bucket has one 256-entry tANS frequency table (not per-position — avoids 1500 tables)
- [ ] 4.3 Implement optional per-bucket bigram table (enabled via NETC_PKT_FLAG_BIGRAM)
- [ ] 4.4 Add bucket frequency tables to dictionary training (netc_dict_train update)
- [ ] 4.5 Write tests: coarse bucket model improves ratio on structured data vs single-table

## 5. Pipeline Integration
- [ ] 5.1 Wire delta pre-pass (field-class aware, per AD-002) before tANS in netc_compress
- [ ] 5.2 Wire delta post-pass after tANS in netc_decompress
- [ ] 5.3 Implement NETC_PKT_FLAG_DELTA flag in packet header
- [ ] 5.4 Verify: delta disabled for packets ≤ 8 bytes (below minimum for useful history)

## 6. Validation
- [ ] 6.1 Benchmark: verify delta improves compression ratio ≥ 10% on WL-001 vs no-delta
- [ ] 6.2 Round-trip test: 100,000 packets per workload with delta enabled
- [ ] 6.3 Verify 95%+ test coverage on delta module
