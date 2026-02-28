# netc C# SDK

.NET 9 P/Invoke wrappers for the netc compression library. Zero-GC `Span<byte>` hot path.

## Requirements

- .NET 9.0+
- `netc.dll` (Windows), `libnetc.so` (Linux), or `libnetc.dylib` (macOS) in the runtime search path

## Build

```bash
# Build the native DLL first
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target netc_shared

# Build the C# SDK
cd sdk/csharp
dotnet build --configuration Release
```

### Run Tests

```bash
# Copy native DLL to test output (Windows example)
cp build/Release/netc.dll sdk/csharp/tests/Netc.Core.Tests/bin/Release/net9.0/

cd sdk/csharp
dotnet test --configuration Release
```

56 xUnit tests covering roundtrip, stateless, error paths, disposal, and trainer.

## API Reference

All types are in the `Netc` namespace.

### `NetcDict`

`IDisposable` dictionary wrapper. Thread-safe for concurrent reads.

```csharp
using Netc;

// Load from binary blob
byte[] blob = File.ReadAllBytes("trained.dict");
using var dict = NetcDict.Load(blob);

// Save
byte[] saved = dict.Save();

// Inspect
byte modelId = dict.ModelId;       // 1-254
bool disposed = dict.IsDisposed;
```

### `NetcContext`

`IDisposable` compression context. NOT thread-safe — use one per connection per thread.

```csharp
using Netc;

// TCP stateful mode with delta + compact headers
using var ctx = NetcContext.Create(dict, NetcMode.Stateful, level: 5,
    extraFlags: 0x08 | 0x10);  // NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR

// Compress (zero-GC Span<byte> API)
byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];
int written = ctx.Compress(src, dst);
// Send dst[..written] over network

// Decompress
byte[] recovered = new byte[65535];
int recoveredLen = ctx.Decompress(dst.AsSpan(0, written), recovered);

// UDP stateless (no context state)
int compLen = NetcContext.CompressStateless(dict, src, dst);
int decLen = NetcContext.DecompressStateless(dict, dst.AsSpan(0, compLen), recovered);

// Utilities
ctx.Reset();                          // Reset ring buffer, keep dict
byte simd = ctx.SimdLevel;            // 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON
NetcStats stats = ctx.GetStats();
double ratio = stats.AverageRatio;
```

### `NetcTrainer`

Corpus management and dictionary training.

```csharp
using Netc;

using var trainer = new NetcTrainer();

// Add packets
foreach (var pkt in capturedPackets)
    trainer.AddPacket(pkt);

int count = trainer.PacketCount;

// Train
using var dict = trainer.Train(modelId: 1);

trainer.Reset();  // Clear corpus
```

### `NetcException`

Maps `netc_result_t` error codes to typed C# exceptions.

```csharp
try
{
    var ctx = NetcContext.Create(dict);
    ctx.Compress(src, dst);
}
catch (NetcException ex)
{
    Console.WriteLine($"Error: {ex.ErrorCode} — {ex.Message}");
}
```

### `NetcResult` Enum

| Value | Name | Description |
|-------|------|-------------|
| 0 | `Ok` | Success |
| -1 | `ErrInvalidArg` | NULL pointer or invalid parameter |
| -2 | `ErrBufferTooSmall` | Output buffer insufficient |
| -3 | `ErrCorrupt` | Corrupted or truncated data |
| -4 | `ErrModelMismatch` | Dict model_id != packet model_id |
| -5 | `ErrNoMem` | Memory allocation failed |
| -6 | `ErrDictRequired` | Operation requires a dictionary |
| -7 | `ErrNotReady` | Context not initialized |

### `NetcMode` Enum

| Value | Name | Description |
|-------|------|-------------|
| 0 | `Stateful` | TCP mode — ring buffer accumulates history |
| 1 | `Stateless` | UDP mode — each packet is independent |

## File Structure

```
sdk/csharp/
├── Netc.Core.sln
├── Netc.Core/
│   ├── Netc.Core.csproj
│   ├── NetcNative.cs          # P/Invoke declarations (22 functions)
│   ├── NetcResult.cs          # enum NetcResult : int
│   ├── NetcMode.cs            # enum NetcMode
│   ├── NetcException.cs       # Exception wrapping NetcResult
│   ├── NetcStats.cs           # Public stats struct
│   ├── NetcDict.cs            # IDisposable dict wrapper
│   ├── NetcContext.cs         # IDisposable context wrapper
│   └── NetcTrainer.cs         # Training helper
├── tests/
│   └── Netc.Core.Tests/
│       ├── Netc.Core.Tests.csproj
│       ├── GlobalUsings.cs
│       ├── Helpers.cs          # Shared dict builder, sample data
│       ├── ResultTests.cs
│       ├── DictTests.cs
│       ├── ContextTests.cs
│       ├── TrainerTests.cs
│       ├── StatelessTests.cs
│       ├── ErrorTests.cs
│       └── DisposalTests.cs
└── README.md
```

## Platform Support

| Platform | Library | Status |
|----------|---------|--------|
| Windows x64 | `netc.dll` | Tested |
| Linux x86_64 | `libnetc.so` | Build supported |
| macOS ARM64 | `libnetc.dylib` | Build supported |
| Android arm64-v8a | `libnetc.so` | Planned |
| iOS arm64 | `libnetc.a` (static) | Planned |

## License

Apache License 2.0 — see [LICENSE](../../LICENSE).
