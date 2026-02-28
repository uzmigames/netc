# Spec: C# SDK — Unity

## ADDED Requirements

### Requirement: Zero GC Pressure in Hot Path
NetcContext.Compress and Decompress SHALL perform no heap allocations when called with pre-allocated Span<byte> output buffers.

#### Scenario: Compress with pre-allocated output
Given a byte[] dst pre-allocated to NetcContext.MaxCompressedSize(srcLen)
When NetcContext.Compress(src.AsSpan(), dst.AsSpan()) is called
Then zero GC allocations SHALL occur (verified by GC.GetAllocatedBytesForCurrentThread)
And the return value SHALL be the actual compressed size in bytes

#### Scenario: No boxing in hot path
Given a hot path calling Compress 1,000,000 times
When measured with dotMemory or JetBrains profiler
Then zero boxing allocations SHALL appear in the Compress/Decompress call stacks

### Requirement: IDisposable Lifecycle
NetcDict and NetcContext SHALL implement IDisposable and release native memory deterministically.

#### Scenario: Context disposed via using statement
Given a NetcContext created in a using block
When the block exits normally or via exception
Then the underlying netc_ctx_t SHALL be freed exactly once
And no native memory leak SHALL be reported by AddressSanitizer on the native side

#### Scenario: Double dispose is safe
Given a NetcContext that has been disposed
When Dispose() is called a second time
Then no exception SHALL be thrown
And no double-free SHALL occur in the native library

### Requirement: Thread-Safe Dictionary
NetcDict SHALL allow concurrent Compress and Decompress calls from multiple NetcContext instances on different threads without synchronization.

#### Scenario: 16 threads compress concurrently
Given one NetcDict and 16 NetcContext instances on 16 threads
When each thread compresses 100,000 packets simultaneously
Then all results SHALL be correct (round-trip verified)
And no ThreadStateException or AccessViolationException SHALL occur

### Requirement: Unity Transport Adapter
NetcMirrorTransport SHALL transparently compress all outgoing packets and decompress all incoming packets without changes to user code.

#### Scenario: Transparent compression in Mirror
Given a Mirror NetworkManager using NetcMirrorTransport
When the client sends a NetworkMessage
Then the transport SHALL compress the payload before sending
And the remote transport SHALL decompress it before delivering to Mirror
And the received message SHALL be byte-for-byte identical to the sent message

#### Scenario: Fallback on incompressible data
Given a Mirror NetworkMessage containing random bytes
When the transport compresses it
Then the passthrough flag SHALL be set (netc guarantee)
And the received payload SHALL be identical to the original

### Requirement: Mobile Platform Support
The native library SHALL be available and functional on Android (arm64-v8a) and iOS (arm64).

#### Scenario: Android arm64 NEON acceleration
Given an Android device with arm64-v8a and NEON support
When NetcContext.Compress is called
Then the NEON SIMD path SHALL be active (verified via netc_ctx_get_simd_level)
And throughput SHALL exceed the generic path by ≥ 50%

#### Scenario: iOS static linking
Given an iOS app built with IL2CPP
When the app runs on an iPhone
Then the static libnetc SHALL be linked correctly (no dlopen)
And Compress/Decompress SHALL function correctly
