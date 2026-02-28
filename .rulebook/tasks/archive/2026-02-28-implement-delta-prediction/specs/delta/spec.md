# Spec: Delta Prediction

## ADDED Requirements

### Requirement: Byte-Level Delta Encoding
The delta encoder SHALL subtract predicted byte values from packet bytes, producing residuals with lower entropy than the original bytes.

#### Scenario: Delta encode/decode round-trip
Given a previous packet P and a current packet C of equal size N
When delta_encode(P, C, residual, N) is called
Then residual[i] = (C[i] - P[i]) mod 256 for all i
And delta_decode(P, residual, recovered, N) SHALL produce recovered[i] = C[i] for all i

#### Scenario: Delta disabled for small packets
Given a packet of size ≤ 64 bytes
When netc_compress is called with NETC_CFG_FLAG_DELTA enabled
Then NETC_PKT_FLAG_DELTA SHALL NOT be set in the output header
And compression proceeds without delta encoding

### Requirement: Ring Buffer History (TCP Mode)
The context SHALL maintain a history ring buffer of previous packets for delta prediction.

#### Scenario: Ring buffer stores previous packet
Given a netc_ctx_t with TCP mode enabled
When packet P1 is compressed
Then P1 bytes SHALL be stored in the ring buffer at the current position
And subsequent packet P2 compression SHALL use P1 as the delta predictor

#### Scenario: Ring buffer wrap-around
Given a ring buffer of size R and a stream of packets totaling > R bytes
When the ring buffer position reaches R
Then it SHALL wrap to 0 without losing data
And subsequent compression SHALL still use the correct predictor

### Requirement: Stateless Delta (UDP Mode)
The UDP stateless codec SHALL use sequence numbers to identify delta predictors.

#### Scenario: In-sequence UDP delta
Given packets with sequence numbers 100 and 101
When packet 101 is compressed with delta against packet 100
Then NETC_PKT_FLAG_DELTA SHALL be set
And context_seq in the packet header SHALL be 100

#### Scenario: Out-of-sequence UDP packet skips delta
Given a received packet with sequence number 105 when last seen was 100
When the packet is compressed
Then NETC_PKT_FLAG_DELTA SHALL NOT be set (gap detected)
And the packet SHALL be compressed without delta prediction

### Requirement: Compression Ratio Improvement
Delta encoding SHALL improve compression ratio for correlated packet streams.

#### Scenario: Delta improves ratio on game state workload
Given a corpus of WL-001 (game state packets) with a trained dictionary
When netc compresses with delta enabled vs delta disabled
Then the compression ratio with delta SHALL be ≤ 0.85× the ratio without delta
<!-- i.e., at least 15% ratio improvement -->
