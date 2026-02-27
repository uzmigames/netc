# Spec: C++ SDK — Unreal Engine 5

## ADDED Requirements

### Requirement: RAII Context Lifecycle
FNetcContext SHALL manage the lifetime of the underlying netc_ctx_t and release it on destruction with no memory leaks.

#### Scenario: Context destroyed on scope exit
Given an FNetcContext created on the stack
When the scope exits
Then netc_ctx_destroy SHALL be called exactly once
And no memory SHALL be leaked (verified by UE5 memory tracking)

#### Scenario: Move semantics
Given FNetcContext A wrapping a valid context
When A is moved into FNetcContext B
Then A SHALL be in a valid-but-empty state (no double-free on destruction)
And B SHALL own the underlying context

### Requirement: Dictionary Thread Safety
FNetcDict SHALL be safe for concurrent reads from multiple FNetcContext instances on different threads.

#### Scenario: Multiple contexts share one dictionary
Given one FNetcDict loaded from file
When 8 FNetcContext instances are created simultaneously on 8 threads
And each compresses 10,000 packets concurrently
Then no data race SHALL occur (verified by ThreadSanitizer)
And all compressed packets SHALL decompress correctly

### Requirement: UE5 TArrayView API
FNetcContext::Compress and Decompress SHALL accept TArrayView<const uint8> as input and write to TArray<uint8> without internal dynamic allocation per call.

#### Scenario: Compress to pre-allocated TArray
Given a TArray<uint8> dst pre-allocated with SetNum(MaxCompressedSize)
When FNetcContext::Compress(src, dst) is called
Then dst.Num() SHALL be set to the actual compressed size
And no heap allocation SHALL occur during the call (verified by FMallocAnsiHook)

### Requirement: UE5 Plugin Build System
The plugin SHALL build correctly with Unreal Build Tool on Win64, Linux x86_64, and macOS ARM64.

#### Scenario: Plugin builds on Win64
Given UE5.3+ and MSVC 2022
When the plugin is added to a project and the project is built
Then compilation SHALL succeed with no warnings at /W4
And libnetc SHALL be linked as a static ThirdParty library

#### Scenario: SIMD flags propagated
Given an AVX2-capable Win64 build machine
When the plugin builds
Then /arch:AVX2 SHALL be passed for netc source files
And the resulting binary SHALL use the AVX2 SIMD path at runtime
