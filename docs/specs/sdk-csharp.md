# netc C# SDK — Unity

```
SDK Version:  0.1.0
Unity:        2022.3 LTS+, 6.x
Runtime:      .NET 6+, Mono 6.12+
Platforms:    Win64, Linux x86_64, macOS ARM64, Android ARM64, iOS ARM64
Depends:      netc core library (libnetc native)
```

---

## Table of Contents

1. [Overview](#1-overview)
2. [Installation](#2-installation)
3. [Core Concepts](#3-core-concepts)
4. [API Reference](#4-api-reference)
   - [NetcDict](#41-netcdict)
   - [NetcContext](#42-netccontext)
   - [NetcTrainer](#43-netctrainer)
   - [NetcResult](#44-netcresult)
   - [NetcMode](#45-netcmode)
5. [Quick Start](#5-quick-start)
6. [Dictionary Workflow](#6-dictionary-workflow)
7. [Stateful Compression](#7-stateful-compression)
8. [Stateless Compression](#8-stateless-compression)
9. [Unity Integration Patterns](#9-unity-integration-patterns)
10. [Thread Safety](#10-thread-safety)
11. [Memory Model](#11-memory-model)
12. [Error Handling](#12-error-handling)
13. [Build System](#13-build-system)
14. [Testing](#14-testing)

---

## 1. Overview

The netc C# SDK wraps the netc C core library (`libnetc`) with idiomatic .NET types:

- **Zero GC pressure** — `Compress` and `Decompress` accept `Span<byte>` and `ReadOnlySpan<byte>`. No heap allocations in the hot path.
- **IDisposable lifecycle** — `NetcDict` and `NetcContext` implement `IDisposable` and release native memory deterministically via `using` blocks.
- **Thread-safe dictionaries** — a single `NetcDict` can be shared read-only across all `NetcContext` instances on all threads without synchronization.
- **Transport-agnostic** — works with any byte buffer (Mirror, FishNet, NGO, ENet, WebRTC, raw sockets, or custom transports). No socket management.
- **Unity adapters** — optional `NetcMirrorTransport` (Mirror) and `NetcFishNetTransport` (FishNet) provided as a separate assembly.

### What the SDK does NOT do

- Manage connections or sockets
- Implement reliable delivery or packet ordering
- Encrypt data
- Operate on transport-layer concepts (TCP/UDP agnostic — compression is buffer-to-buffer)

---

## 2. Installation

### 2.1 Unity Package Manager

Add to `Packages/manifest.json`:

```json
{
  "dependencies": {
    "com.netc.core": "file:../sdk/csharp/Netc.Core",
    "com.netc.unity": "file:../sdk/csharp/Netc.Unity"
  }
}
```

Native libraries (`netc_win64.dll`, `netc_linux64.so`, `netc_macos_arm64.dylib`) are bundled under `Plugins/` with correct platform metadata.

---

## 3. Core Concepts

### 3.1 Dictionary

A `NetcDict` holds a trained probability model. It is created once from a representative packet corpus and then shared read-only across all compression contexts. Dictionaries are versioned with an 8-bit `model_id` enabling rolling upgrades (server accepts model N and N+1 simultaneously).

### 3.2 Context

A `NetcContext` holds per-connection state: the ring buffer for stateful (ordered channel) mode, the previous packet for stateless delta prediction, and per-context working memory. One context per logical connection.

### 3.3 Stateful vs Stateless Mode

| Mode | Use case | History |
|------|----------|---------|
| `NetcMode.Stateful` | Ordered reliable channel (guaranteed delivery, ordered) | Ring buffer accumulates across calls |
| `NetcMode.Stateless` | Independent payloads (any order, any transport) | Previous packet only; drops delta if sequence gap |

The mode is a property of how the caller uses the context — not of the transport protocol.

### 3.4 Passthrough Guarantee

If compression would expand the payload, netc emits the original bytes with a passthrough flag. Output is always `≤ input size + NetcContext.MaxOverheadBytes` (8 bytes). Callers can always allocate `src.Length + NetcContext.MaxOverheadBytes` and the buffer will never overflow.

---

## 4. API Reference

### 4.1 NetcDict

```csharp
public sealed class NetcDict : IDisposable
{
    // Load a pre-trained dictionary from bytes (e.g. embedded resource or file).
    public static NetcDict Load(ReadOnlySpan<byte> data);

    // Save the dictionary to a byte array (for persistence or distribution).
    public byte[] Save();

    // The model_id embedded in this dictionary (0–254; 255 reserved).
    public byte ModelId { get; }

    // True if the dictionary has been disposed.
    public bool IsDisposed { get; }

    public void Dispose();
}
```

### 4.2 NetcContext

```csharp
public sealed class NetcContext : IDisposable
{
    // Maximum extra bytes compression can add over the original (header only).
    public const int MaxOverheadBytes = 8;

    // Create a context bound to a dictionary and mode.
    public static NetcContext Create(NetcDict dict, NetcMode mode = NetcMode.Stateful);

    // Compress src into dst. Returns compressed size. dst must be >= src.Length + MaxOverheadBytes.
    // Zero GC allocations when using Span<byte> overloads.
    public int Compress(ReadOnlySpan<byte> src, Span<byte> dst);

    // Decompress src into dst. Returns decompressed size.
    public int Decompress(ReadOnlySpan<byte> src, Span<byte> dst);

    // Helper: returns the minimum safe dst buffer size for a given src length.
    public static int MaxCompressedSize(int srcLength) => srcLength + MaxOverheadBytes;

    // Reset per-connection state (ring buffer, sequence counter) without releasing memory.
    // Call on connection reset or reconnect.
    public void Reset();

    // True if the context has been disposed.
    public bool IsDisposed { get; }

    public void Dispose();
}
```

### 4.3 NetcTrainer

```csharp
public sealed class NetcTrainer : IDisposable
{
    // Add a packet to the training corpus.
    public void AddPacket(ReadOnlySpan<byte> packet);

    // Add multiple packets from a collection.
    public void AddPackets(IEnumerable<ReadOnlyMemory<byte>> packets);

    // Train and produce a NetcDict from the accumulated corpus.
    // modelId: 1–254 (0 reserved for passthrough, 255 reserved).
    public NetcDict Train(byte modelId = 1);

    public void Dispose();
}
```

### 4.4 NetcResult

```csharp
public enum NetcResult
{
    Ok               =  0,
    ErrCtxNull       = -1,
    ErrBufSmall      = -2,
    ErrTooBig        = -3,
    ErrCorrupt       = -4,
    ErrDictMismatch  = -5,
    ErrNoDict        = -6,
    ErrInvalidArg    = -7,
}
```

### 4.5 NetcMode

```csharp
public enum NetcMode
{
    Stateful   = 1,  // History accumulates (ordered reliable channel)
    Stateless  = 2,  // Each call independent (any delivery order)
}
```

---

## 5. Quick Start

```csharp
// 1. Train a dictionary from representative packets (done once, offline).
using var trainer = new NetcTrainer();
foreach (var packet in myPacketCorpus)
    trainer.AddPacket(packet);
using NetcDict dict = trainer.Train(modelId: 1);
File.WriteAllBytes("netc.dict", dict.Save());

// 2. Load the dictionary at runtime (shared across all connections).
var dictBytes = File.ReadAllBytes("netc.dict");
var sharedDict = NetcDict.Load(dictBytes);  // Keep alive for the session lifetime

// 3. Create one context per connection.
using var ctx = NetcContext.Create(sharedDict, NetcMode.Stateless);

// 4. Compress a packet (zero GC allocation with Span).
byte[] src = GetPacketBytes();
byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];

int compressedLen = ctx.Compress(src, dst);
Send(dst.AsSpan(0, compressedLen));

// 5. Decompress on the remote side.
byte[] received = Receive();
byte[] decompressed = new byte[src.Length];  // original_size from header
ctx.Decompress(received, decompressed);
```

---

## 6. Dictionary Workflow

### 6.1 Training

Train the dictionary from a representative packet capture before shipping:

```csharp
using var trainer = new NetcTrainer();

// Feed packets that represent typical in-game traffic.
// Aim for ≥ 10,000 packets for a good probability model.
foreach (var packetBytes in capturedPackets)
    trainer.AddPacket(packetBytes);

using NetcDict dict = trainer.Train(modelId: 1);
byte[] serialized = dict.Save();
File.WriteAllBytes("Assets/Resources/netc.dict", serialized);
```

### 6.2 Rolling Dictionary Upgrade

When shipping a new model:

1. Train with `modelId: 2`
2. Deploy server that accepts both `modelId: 1` and `modelId: 2`
3. Roll out clients with the new dictionary
4. After all clients upgraded, retire `modelId: 1`

The `model_id` field in the packet header (RFC-001 §9.1) ensures the server decompresses with the correct model version.

---

## 7. Stateful Compression

Use `NetcMode.Stateful` when payloads arrive in order on a reliable channel. History accumulates across calls, improving compression ratio via inter-packet delta prediction.

```csharp
using var ctx = NetcContext.Create(sharedDict, NetcMode.Stateful);

// Send side — called in order for each packet on this connection.
int len = ctx.Compress(entityStatePacket, dst);
transport.Send(dst, len);

// Receive side — must decompress in the same order.
ctx.Decompress(received, decompressed);

// On disconnect / reconnect: reset the context state.
ctx.Reset();
```

**Guarantee**: Both ends must process packets in the same order and call `Reset()` in sync (e.g., on reconnect). If the channel loses ordering, use `NetcMode.Stateless` instead.

---

## 8. Stateless Compression

Use `NetcMode.Stateless` when each payload is independent (no ordering guarantee, lossy channels, or custom retransmission). Delta prediction uses only the immediately previous packet; if the `context_seq` field shows a gap, delta is skipped automatically.

```csharp
using var ctx = NetcContext.Create(sharedDict, NetcMode.Stateless);

// Each Compress/Decompress call is independent.
ctx.Compress(snapshot, dst);
// ...
ctx.Decompress(received, decompressed);
```

---

## 9. Unity Integration Patterns

### 9.1 NetcMirrorTransport (Mirror Networking)

`NetcMirrorTransport` wraps any Mirror transport and transparently compresses all packets:

```csharp
// In the Inspector: set Inner to your KcpTransport / TelepathyTransport / etc.
public class NetcMirrorTransport : Transport
{
    [SerializeField] private Transport inner;
    [SerializeField] private TextAsset dictAsset;

    private NetcDict _dict;
    private readonly ConcurrentDictionary<int, NetcContext> _ctxByConn = new();

    private void Awake()
    {
        _dict = NetcDict.Load(dictAsset.bytes);
    }

    public override void ClientSend(ArraySegment<byte> segment, int channel)
    {
        Span<byte> dst = stackalloc byte[NetcContext.MaxCompressedSize(segment.Count)];
        int len = _clientCtx.Compress(segment, dst);
        inner.ClientSend(new ArraySegment<byte>(dst.ToArray(), 0, len), channel);
    }

    // ... ServerSend, OnClientDataReceived, OnServerDataReceived follow same pattern
}
```

### 9.2 Pre-allocating Output Buffers

Avoid `stackalloc` for large packets (> 1024 bytes). Use a thread-local pool:

```csharp
[ThreadStatic]
private static byte[] _sendBuffer;

private static byte[] GetSendBuffer(int minSize)
{
    if (_sendBuffer == null || _sendBuffer.Length < minSize)
        _sendBuffer = new byte[minSize];
    return _sendBuffer;
}

// In compress path:
byte[] buf = GetSendBuffer(NetcContext.MaxCompressedSize(src.Length));
int len = ctx.Compress(src, buf);
```

### 9.3 Unity Lifecycle

```csharp
public class NetcManager : MonoBehaviour
{
    private NetcDict _dict;
    private NetcContext _ctx;

    private void Awake()
    {
        var asset = Resources.Load<TextAsset>("netc");
        _dict = NetcDict.Load(asset.bytes);
        _ctx = NetcContext.Create(_dict, NetcMode.Stateless);
    }

    private void OnDestroy()
    {
        _ctx?.Dispose();
        _dict?.Dispose();
    }
}
```

---

## 10. Thread Safety

| Object | Thread Safety |
|--------|---------------|
| `NetcDict` | **Fully thread-safe** — immutable after `Train()`; any number of threads may reference it concurrently without locks. |
| `NetcContext` | **Not thread-safe** — one context per thread per connection. Create separate contexts per thread. |
| `NetcTrainer` | **Not thread-safe** — use from a single thread during training. |

**Pattern**: One shared `NetcDict`, one `NetcContext` per connection per thread.

```csharp
// Correct: shared dict, per-connection context
var dict = NetcDict.Load(dictBytes);  // shared
var ctx1 = NetcContext.Create(dict);  // connection A
var ctx2 = NetcContext.Create(dict);  // connection B

// Incorrect: never share a context across threads
// var ctx = NetcContext.Create(dict);
// Task.Run(() => ctx.Compress(...));  // WRONG
```

---

## 11. Memory Model

### 12.1 Native Memory

All native memory is allocated when `NetcContext.Create()` is called. No native heap activity occurs during `Compress()` or `Decompress()`. The context pre-allocates:

- Working arena: `2 × MaxPacketSize` = 3,000 bytes (configurable)
- tANS decode table: 16 KB (4,096 entries × 4 bytes)
- Ring buffer (stateful mode): configurable, default 64 KB

### 12.2 GC Pressure

`Compress(ReadOnlySpan<byte>, Span<byte>)` and `Decompress(ReadOnlySpan<byte>, Span<byte>)` perform **zero heap allocations**. Use `stackalloc` for small packets (≤ 1 KB) or thread-local byte arrays for large packets to keep output buffers off the GC heap.

```csharp
// Small packet — stack allocated (safe up to ~1KB)
Span<byte> dst = stackalloc byte[NetcContext.MaxCompressedSize(src.Length)];
int len = ctx.Compress(src, dst);

// Large packet — thread-local pool
byte[] dst = GetSendBuffer(NetcContext.MaxCompressedSize(src.Length));
int len = ctx.Compress(src, dst);
```

### 12.3 Disposal

Always dispose `NetcContext` and `NetcDict` when no longer needed. Using `IDisposable` via `using` is the recommended pattern:

```csharp
using var dict = NetcDict.Load(dictBytes);
using var ctx = NetcContext.Create(dict);
// Automatically disposed when block exits
```

---

## 12. Error Handling

All errors are signaled by throwing `NetcException` (wraps `NetcResult`):

```csharp
public class NetcException : Exception
{
    public NetcResult Result { get; }
    public NetcException(NetcResult result)
        : base($"netc error: {result}") => Result = result;
}
```

Common cases:

| Result | Cause | Action |
|--------|-------|--------|
| `ErrBufSmall` | `dst` too small | Always allocate `MaxCompressedSize(src.Length)` |
| `ErrCorrupt` | Malformed compressed data | Log + discard packet; do not crash |
| `ErrDictMismatch` | Sender used different `model_id` | Initiate rolling upgrade handshake |
| `ErrNoDict` | Context created without dictionary | Always pass a valid `NetcDict` |

```csharp
try
{
    int len = ctx.Decompress(received, dst);
    ProcessPacket(dst.AsSpan(0, len));
}
catch (NetcException ex) when (ex.Result == NetcResult.ErrCorrupt)
{
    _logger.LogWarning("Corrupt packet from peer {Id}, discarding", peerId);
}
```

---

## 13. Build System

### 13.1 Project Structure

```
sdk/csharp/
├── Netc.Core/                  # Core C# wrapper (no Unity deps)
│   ├── Netc.Core.csproj
│   ├── NetcDict.cs
│   ├── NetcContext.cs
│   ├── NetcTrainer.cs
│   ├── NetcException.cs
│   ├── NetcResult.cs
│   ├── NetcMode.cs
│   └── Native/
│       ├── NetcNative.cs       # P/Invoke declarations
│       └── Plugins/            # Native .dll/.so/.dylib per platform
├── Netc.Unity/                 # Unity-specific adapters
│   ├── Netc.Unity.asmdef
│   ├── NetcMirrorTransport.cs
│   ├── NetcFishNetTransport.cs
│   └── NetcManager.cs
└── tests/                      # xUnit test projects
```

### 13.2 P/Invoke Declarations

```csharp
internal static class NetcNative
{
    private const string Lib = "netc";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe IntPtr netc_ctx_create(netc_cfg_t* cfg);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void netc_ctx_destroy(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int netc_compress(
        IntPtr ctx,
        byte* src, uint src_size,
        byte* dst, uint dst_cap,
        uint* dst_size);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern unsafe int netc_decompress(
        IntPtr ctx,
        byte* src, uint src_size,
        byte* dst, uint dst_cap,
        uint* dst_size);
}
```

### 13.3 CMake Integration

The C# SDK build is driven by CMake:

```bash
cmake -B build -DNETC_BUILD_CSHARP_SDK=ON
cmake --build build --target netc_csharp
```

This produces the native shared library in `sdk/csharp/Netc.Core/Native/Plugins/`.

---

## 14. Testing

### 14.1 Test Projects

```
sdk/csharp/
└── tests/
    ├── Netc.Core.Tests/
    │   ├── DictTests.cs           # Load/Save, model_id, CRC validation
    │   ├── ContextTests.cs        # Compress/Decompress round-trip, all workloads
    │   ├── PassthroughTests.cs    # Incompressible data passthrough guarantee
    │   ├── StatefulTests.cs       # Ring buffer history, Reset()
    │   ├── StatelessTests.cs      # Stateless delta, sequence gap handling
    │   ├── ThreadSafetyTests.cs   # 16 threads × 100k packets
    │   ├── DisposalTests.cs       # IDisposable, double-dispose safety
    │   └── ErrorHandlingTests.cs  # All NetcResult codes, corrupt input
    └── Netc.Unity.Tests/
        └── MirrorTransportTests.cs
```

### 14.2 Coverage Requirement

Test coverage must be ≥ 95% on `Netc.Core` (measured with Coverlet). The CI pipeline enforces this threshold.

### 14.3 Zero-GC Verification

```csharp
[Fact]
public void Compress_Span_ZeroAllocations()
{
    long before = GC.GetAllocatedBytesForCurrentThread();
    for (int i = 0; i < 1_000_000; i++)
        _ctx.Compress(_src, _dst);
    long after = GC.GetAllocatedBytesForCurrentThread();
    Assert.Equal(before, after);
}
```

---

*End of C# SDK (Unity) specification. See [sdk-godot.md](sdk-godot.md) for the Godot 4 GDExtension SDK.*
