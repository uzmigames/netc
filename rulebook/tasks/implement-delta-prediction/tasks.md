## 1. Delta Encoder/Decoder
- [ ] 1.1 Implement delta_encode(prev, curr, residual, size) — byte subtraction with wraparound
- [ ] 1.2 Implement delta_decode(prev, residual, curr, size) — byte addition reconstruction
- [ ] 1.3 Implement position-weighted delta (header bytes vs payload bytes different weights)
- [ ] 1.4 Write tests: delta encode/decode round-trip (test_delta.c)

## 2. Ring Buffer History (TCP Mode)
- [ ] 2.1 Add ring buffer to netc_ctx_t (configurable size, default 64KB)
- [ ] 2.2 Implement ring buffer write on each compressed packet
- [ ] 2.3 Implement ring buffer read for delta prediction lookup
- [ ] 2.4 Write tests: ring buffer correctness and wrap-around

## 3. Stateless Delta (UDP Mode)
- [ ] 3.1 Implement sequence number tracking for UDP delta
- [ ] 3.2 Implement out-of-order packet detection (skip delta if sequence gap)
- [ ] 3.3 Write tests: UDP delta with sequence numbers

## 4. Bigram Context Model
- [ ] 4.1 Implement per-position probability table (256 contexts × 256 symbols)
- [ ] 4.2 Integrate bigram model into ANS probability table selection
- [ ] 4.3 Add bigram tables to dictionary training (netc_dict_train update)
- [ ] 4.4 Write tests: bigram model improves ratio on structured data

## 5. Pipeline Integration
- [ ] 5.1 Wire delta pre-pass before ANS in netc_compress
- [ ] 5.2 Wire delta post-pass after ANS in netc_decompress
- [ ] 5.3 Implement NETC_PKT_FLAG_DELTA flag in packet header
- [ ] 5.4 Verify: delta disabled for packets ≤ 64 bytes (insufficient history)

## 6. Validation
- [ ] 6.1 Benchmark: verify delta improves compression ratio ≥ 10% on WL-001 vs no-delta
- [ ] 6.2 Round-trip test: 100,000 packets per workload with delta enabled
- [ ] 6.3 Verify 95%+ test coverage on delta module
