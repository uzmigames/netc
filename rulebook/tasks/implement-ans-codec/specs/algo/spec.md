# Spec: ANS Codec

## ADDED Requirements

### Requirement: rANS Encoder Correctness
The rANS encoder SHALL produce output that the rANS decoder can recover to the exact original byte sequence.

#### Scenario: Round-trip small corpus
Given a trained dictionary and a packet of N bytes (64 < N ≤ 1500)
When netc_compress is called followed by netc_decompress
Then the decompressed output SHALL be byte-for-byte identical to the input
And the function SHALL return NETC_OK

#### Scenario: rANS does not expand high-entropy data
Given a packet of 128 random bytes (entropy ≈ 8 bits/byte)
When netc_compress is called
Then NETC_PKT_FLAG_PASSTHRU SHALL be set (passthrough activated)
And output size SHALL be ≤ 128 + NETC_HEADER_SIZE

### Requirement: tANS Small Packet Codec
The tANS encoder/decoder SHALL handle packets of 8–64 bytes correctly.

#### Scenario: Round-trip 8-byte minimum packet
Given a packet of exactly 8 bytes
When netc_compress is called
Then the tANS path SHALL be selected (NETC_PKT_FLAG_SMALL set)
And netc_decompress SHALL return the original bytes

#### Scenario: tANS decode table fits in L1 cache
Given a tANS decode table with 12-bit index (4096 entries × 4 bytes = 16 KB)
When the table is built at dictionary load time
Then table size SHALL be ≤ 16 KB

### Requirement: Dictionary Validation
The library SHALL reject dictionaries with invalid checksums.

#### Scenario: Corrupt dictionary rejected
Given a netc_dict_t with a modified byte after training
When netc_dict_load is called
Then the function SHALL return NETC_ERR_DICT_INVALID
And no context SHALL be created from the corrupt dictionary

### Requirement: Algorithm Selection
The library SHALL automatically select the optimal coding path per packet.

#### Scenario: Small packet uses tANS
Given a packet of size ≤ 64 bytes
When netc_compress is called
Then the NETC_PKT_FLAG_SMALL flag SHALL be set in the output header

#### Scenario: Large packet uses rANS
Given a packet of size > 64 bytes
When netc_compress is called
And the packet entropy is < 6 bits/byte
Then the NETC_PKT_FLAG_SMALL flag SHALL NOT be set
And rANS encoding SHALL be used
