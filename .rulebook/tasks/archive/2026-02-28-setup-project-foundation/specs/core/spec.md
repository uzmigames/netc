# Spec: Project Foundation

## ADDED Requirements

### Requirement: Public API Header
The library SHALL expose a complete public API through a single header file `include/netc.h`.

#### Scenario: API completeness
Given a C11 project including netc.h
When the project is compiled
Then all types, functions, and constants defined in RFC-001 §10 SHALL be available without additional headers

#### Scenario: Self-contained header
Given netc.h
When included in isolation (no other netc headers)
Then compilation SHALL succeed with standard libc only

### Requirement: Build System
The project SHALL build with CMake 3.20+ and produce a static library `libnetc.a` and shared library `libnetc.so`.

#### Scenario: Release build
Given CMake configuration with -DCMAKE_BUILD_TYPE=Release
When cmake --build is executed
Then compilation SHALL succeed with -O3 optimization
And SIMD flags SHALL be auto-detected and applied when available
And the resulting library SHALL pass all unit tests

#### Scenario: Debug build
Given CMake configuration with -DCMAKE_BUILD_TYPE=Debug
When cmake --build is executed
Then compilation SHALL succeed with -g -fsanitize=address,undefined
And all tests SHALL pass under sanitizers

### Requirement: Passthrough Baseline
The library SHALL implement lossless passthrough compression as a baseline.

#### Scenario: Compress incompressible data
Given a packet of N bytes
When netc_compress is called with a trained dictionary
And the compressed output would exceed N bytes
Then the function SHALL set NETC_PKT_FLAG_PASSTHRU in the output header
And the output SHALL contain the original bytes verbatim
And dst_size SHALL be N + NETC_HEADER_SIZE

#### Scenario: Decompress passthrough packet
Given a compressed packet with NETC_PKT_FLAG_PASSTHRU set
When netc_decompress is called
Then dst SHALL contain the original bytes
And dst_size SHALL equal the original packet size
And the function SHALL return NETC_OK

### Requirement: Error Handling Contract
All public API functions SHALL return netc_result_t and MUST NOT crash on invalid input.

#### Scenario: NULL context
Given ctx = NULL
When netc_compress(NULL, src, size, dst, cap, &out) is called
Then the function SHALL return NETC_ERR_CTX_NULL
And no memory SHALL be written

#### Scenario: Buffer too small
Given dst_cap = 0
When netc_compress is called
Then the function SHALL return NETC_ERR_BUF_SMALL
And no memory outside dst SHALL be written

#### Scenario: Input too large
Given src_size > NETC_MAX_PACKET_SIZE (65535)
When netc_compress is called
Then the function SHALL return NETC_ERR_TOOBIG
