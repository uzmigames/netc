# Spec: Security Hardening and Fuzz Testing

## ADDED Requirements

### Requirement: Decompressor Output Bound Enforcement
The decompressor SHALL never write beyond dst_cap bytes regardless of the value in original_size.

#### Scenario: Malicious original_size rejected
Given a crafted compressed packet claiming original_size = 65535
And dst_cap = 128
When netc_decompress is called
Then at most 128 bytes SHALL be written to dst
And the function SHALL return NETC_ERR_BUF_SMALL

#### Scenario: Truncated input does not crash
Given a compressed packet with 10 bytes truncated from the end
When netc_decompress is called
Then the function SHALL return NETC_ERR_CORRUPT
And no memory outside dst[0..dst_cap-1] SHALL be written

### Requirement: ANS State Bounds Validation
The ANS decoder SHALL validate its internal state on every symbol decode.

#### Scenario: Corrupt ANS state detected
Given a compressed packet with a corrupt ANS state word
When netc_decompress is called
Then NETC_ERR_CORRUPT SHALL be returned
And no out-of-bounds memory SHALL be accessed

### Requirement: Fuzz Testing Coverage
The library SHALL survive ≥ 10,000,000 fuzz iterations without crashes, hangs, or sanitizer errors.

#### Scenario: libFuzzer finds no crashes
Given arbitrary bytes as input to netc_decompress
When the libFuzzer target runs for 10,000,000 iterations
Then no crash, hang (> 1 second), or sanitizer error SHALL occur

#### Scenario: Fuzz dictionary loading
Given arbitrary bytes as input to netc_dict_load
When the libFuzzer target runs for 10,000,000 iterations
Then no crash or sanitizer error SHALL occur
And all invalid dictionaries SHALL return NETC_ERR_DICT_INVALID

### Requirement: Test Coverage Threshold
All source files in src/ SHALL achieve ≥ 95% line coverage as measured by gcov or llvm-cov.

#### Scenario: Coverage gate enforced in CI
Given the full test suite run with coverage instrumentation
When make coverage is executed
Then the report SHALL show ≥ 95% line coverage for all files in src/
And the CI pipeline SHALL fail if any file is below 95%

### Requirement: Address Sanitizer Clean
The library SHALL produce zero ASan/UBSan errors under the full test suite.

#### Scenario: No memory errors under ASan
Given a build with -fsanitize=address,undefined
When the full test suite is executed
Then zero AddressSanitizer errors SHALL be reported
And zero UndefinedBehaviorSanitizer errors SHALL be reported
