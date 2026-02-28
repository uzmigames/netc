# Spec Delta: tANS Codec Throughput Optimization

## MODIFIED Requirements

### Requirement: Encode Entry Structure
The tANS encode entry SHALL contain `freq`, `lower` (pre-computed threshold), `nb_hi`,
and `cumul` fields so that a single aligned load retrieves all data needed for the
normalization step, eliminating the separate `freq.freq[]` array lookup in the hot path.

#### Scenario: Single load covers freq and nb_hi
Given a built tANS table
When the encoder processes a symbol
Then `encode[sym].freq` and `encode[sym].nb_hi` are fetched in one cache-line access
And no separate lookup into `netc_freq_table_t.freq[]` is required per symbol

#### Scenario: Pre-computed lower threshold
Given a built tANS table
When the encoder evaluates whether to use `nb_hi` or `nb_hi - 1`
Then `encode[sym].lower` contains the pre-computed value `freq << nb_hi`
And no shift operation is performed in the encode hot loop

### Requirement: encode_state Stores Final State
The `encode_state[]` array SHALL store `TABLE_SIZE + slot` (the complete next state X)
rather than just the raw slot index, so the encoder hot path performs a direct assignment
`X = encode_state[cumul + j]` without an additional addition.

#### Scenario: Direct state assignment in encode loop
Given a symbol with cumulative index `cumul` and rank `j`
When the encoder computes the next state
Then `X = tbl->encode_state[encode[sym].cumul + j]` yields a value in `[TABLE_SIZE, 2*TABLE_SIZE)`
And no `+ TABLE_SIZE` addition is needed in the hot path

### Requirement: Word-at-a-time Bitstream Writer
The bitstream writer SHALL flush 4 bytes at once when the accumulator holds ≥ 32 bits,
replacing the byte-at-a-time loop, so the flush branch fires at most once per 3–4
symbols rather than once per symbol.

#### Scenario: No per-byte loop in hot path
Given a bitstream writer with 12 bits in the accumulator
When `netc_bsw_write` is called with 12 more bits (total 24 bits)
Then no flush occurs (bits < 32)
And the accumulator holds 24 bits

#### Scenario: 4-byte flush when accumulator fills
Given a bitstream writer with 28 bits in the accumulator
When `netc_bsw_write` is called with 10 more bits (total 38 bits)
Then exactly 4 bytes are written to the output buffer
And the accumulator retains 6 bits (38 - 32)

#### Scenario: Overflow guard in flush only
Given a bitstream writer near the end of its buffer
When `netc_bsw_flush` is called
Then the overflow check is performed once
And the per-symbol write path contains no overflow branch

### Requirement: Decode Loop Without Per-Symbol Bounds Check
The tANS decode inner loop SHALL NOT check `X < TABLE_SIZE || X >= 2*TABLE_SIZE` on
every iteration; this check SHALL occur only once on the initial state at function entry.

#### Scenario: Initial state validated once
Given `netc_tans_decode` is called with `initial_state`
When `initial_state` is out of `[TABLE_SIZE, 2*TABLE_SIZE)`
Then the function returns -1 immediately before entering the decode loop

#### Scenario: No branch on state in decode loop
Given a valid initial state within `[TABLE_SIZE, 2*TABLE_SIZE)`
When the decoder processes N symbols
Then the state bounds check executes exactly once (at entry), not N times
And the decode loop body contains no conditional return on X value
