# Spec: SIMD Acceleration

## ADDED Requirements

### Requirement: Runtime SIMD Dispatch
The library SHALL detect SIMD capabilities at runtime and select the optimal implementation without user intervention.

#### Scenario: Auto-detect AVX2 on capable CPU
Given a CPU that supports AVX2
When netc_ctx_create is called with simd_level = 0 (auto)
Then the context SHALL use the AVX2 implementation for bulk operations
And the selected level SHALL be accessible via netc_ctx_get_simd_level()

#### Scenario: Graceful fallback on unsupported SIMD
Given a CPU that does not support SSE4.2 or AVX2
When netc_ctx_create is called
Then the generic C fallback SHALL be selected automatically
And all operations SHALL produce correct output

#### Scenario: Manual override
Given netc_cfg_t with simd_level = 1 (generic)
When netc_ctx_create is called on an AVX2-capable CPU
Then the generic path SHALL be forced
And output SHALL be identical to the AVX2 path

### Requirement: Output Correctness Across Paths
All SIMD implementations SHALL produce byte-for-byte identical output to the generic reference implementation.

#### Scenario: Cross-path consistency
Given the same packet and dictionary
When compressed via generic path and via SSE4.2 path
Then the compressed bitstreams SHALL be identical byte-for-byte

#### Scenario: Decompress cross-path
Given a packet compressed via AVX2 path
When decompressed via generic path
Then the output SHALL be identical to the original packet

### Requirement: SIMD Throughput Targets
With AVX2 enabled, the library SHALL meet the following performance targets.

#### Scenario: AVX2 compression throughput
Given a corpus of WL-001 (game state, 64 bytes) on an AVX2-capable CPU
When netc compresses 1,000,000 packets
Then throughput SHALL be ≥ 3 GB/s

#### Scenario: SIMD decompression Mpps
Given a corpus of WL-001 (64-byte packets) on a SIMD-capable CPU
When netc decompresses 1,000,000 packets
Then throughput SHALL be ≥ 20 Mpps

### Requirement: No SIMD Alignment Faults
All SIMD loads and stores SHALL be safe for unaligned packet buffers.

#### Scenario: Unaligned input buffer
Given a packet buffer starting at an odd memory address
When netc_compress is called
Then no SIMD alignment fault SHALL occur
And the result SHALL be correct
