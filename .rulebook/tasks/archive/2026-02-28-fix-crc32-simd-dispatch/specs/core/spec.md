# Spec Delta: CRC32 SIMD Dispatch

## MODIFIED Requirements

### Requirement: CRC32 Hardware Acceleration
The CRC32 checksum computation SHALL use the SIMD dispatch table (`netc_simd_ops_t.crc32_update`) when a hardware-accelerated path is available, falling back to the software table-based implementation on the generic path.

#### Scenario: CRC32 uses SSE4.2 intrinsics on supported CPU
Given a CPU with SSE4.2 support
When `netc_simd_ops_init` is called with `NETC_SIMD_LEVEL_AUTO`
Then `ops->crc32_update` SHALL point to the SSE4.2 hardware implementation

#### Scenario: CRC32 falls back to software on unsupported CPU
Given a CPU without SSE4.2 support
When `netc_simd_ops_init` is called with `NETC_SIMD_LEVEL_AUTO`
Then `ops->crc32_update` SHALL point to `netc_crc32_update_generic`

#### Scenario: All CRC32 paths produce identical output
Given the same input data and initial CRC value
When CRC32 is computed via the generic path and the SSE4.2 path
Then both MUST return the same checksum value

### Requirement: CRC32 Call Site Consolidation
All internal CRC32 call sites (including `netc_dict.c`) MUST route through the SIMD dispatch table rather than calling the software-only `netc_crc32()` directly.

#### Scenario: Dictionary checksum uses SIMD-dispatched CRC32
Given a dictionary being trained or loaded
When the dictionary checksum is computed
Then it SHALL use the SIMD-dispatched CRC32 function instead of the software-only path

### Requirement: Single CRC32 Table Source
The project MUST NOT maintain duplicate CRC32 lookup tables. The generic software fallback SHALL share or reference a single canonical table.

#### Scenario: No duplicate CRC32 tables in codebase
Given the compiled project
When inspecting `netc_simd_generic.c` and `netc_crc32.c`
Then only one CRC32 lookup table definition MUST exist
