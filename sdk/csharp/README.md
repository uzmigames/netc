# netc C# SDK

.NET 9 P/Invoke wrappers for the netc compression library. Zero-GC `Span<byte>` hot path. Works with any .NET project, including Unity 2021+.

## Requirements

- .NET 9.0+ (standalone) or Unity 2021.3+ (Mono/IL2CPP)
- Native library: `netc.dll` (Windows), `libnetc.so` (Linux), `libnetc.dylib` (macOS)

## Build from Source

```bash
# 1. Build native shared library
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target netc_shared

# 2. Build C# SDK
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

---

## Quick Start

```csharp
using Netc;

// 1. Train a dictionary from captured packets
using var trainer = new NetcTrainer();
foreach (var pkt in capturedPackets)
    trainer.AddPacket(pkt);
using var dict = trainer.Train(modelId: 1);

// 2. Save dictionary for reuse
byte[] blob = dict.Save();
File.WriteAllBytes("game_traffic.dict", blob);

// 3. Compress (TCP stateful, delta + compact headers)
using var ctx = NetcContext.Create(dict, NetcMode.Stateful, level: 5,
    extraFlags: 0x08 | 0x10); // DELTA | COMPACT_HDR

byte[] dst = new byte[NetcContext.MaxCompressedSize(src.Length)];
int written = ctx.Compress(src, dst);
// send dst[..written] over network...

// 4. Decompress on the other side
byte[] recovered = new byte[65535];
int recoveredLen = ctx.Decompress(dst.AsSpan(0, written), recovered);
```

---

## Integration: Unity

### 1. Build Native Libraries

Build `netc` as a shared library for each target platform:

```bash
# Windows x64
cmake -B build-win64 -G "Visual Studio 17 2022" -A x64
cmake --build build-win64 --config Release --target netc_shared
# Output: build-win64/Release/netc.dll

# Linux x64 (dedicated server)
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target netc_shared
# Output: build-linux/libnetc.so

# macOS (Apple Silicon)
cmake -B build-macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-macos --target netc_shared
# Output: build-macos/libnetc.dylib
```

### 2. Unity Project Structure

Copy the native libraries and C# source files into your Unity project:

```
Assets/
├── Plugins/
│   └── netc/
│       ├── x86_64/
│       │   ├── netc.dll              # Windows x64
│       │   ├── libnetc.so            # Linux x64
│       │   └── netc.bundle           # macOS (rename .dylib)
│       └── Android/
│           └── arm64-v8a/
│               └── libnetc.so        # Android ARM64
├── Scripts/
│   └── Netc/
│       ├── NetcNative.cs
│       ├── NetcResult.cs
│       ├── NetcMode.cs
│       ├── NetcException.cs
│       ├── NetcStats.cs
│       ├── NetcDict.cs
│       ├── NetcContext.cs
│       └── NetcTrainer.cs
└── StreamingAssets/
    └── game_traffic.dict              # Pre-trained dictionary
```

**Important**: In Unity's plugin import settings, set each native library's platform filter:
- `netc.dll` → Windows x64
- `libnetc.so` (Plugins/netc/x86_64/) → Linux x64
- `netc.bundle` → macOS
- `libnetc.so` (Plugins/netc/Android/) → Android ARM64

### 3. Unity Compatibility Notes

The C# SDK source files (`Netc.Core/*.cs`) work in Unity with minor adjustments:

- **Namespace declaration**: Change `namespace Netc;` (file-scoped) to `namespace Netc { ... }` if using Unity < 2023 (C# 10 required for file-scoped namespaces)
- **`nint`/`nuint`**: Available in Unity 2021.2+ (C# 9). For older versions, replace with `IntPtr` / `UIntPtr`
- **`ReadOnlySpan<byte>`**: Available in Unity 2021.2+ via `System.Memory`. For older versions, use `byte[]` with `unsafe fixed`

### 4. Usage in Unity (Mirror/FishNet/NGO)

```csharp
using UnityEngine;
using Netc;

public class NetcCompression : MonoBehaviour
{
    private NetcDict _dict;
    private NetcContext _encCtx;
    private NetcContext _decCtx;

    // Pre-allocated buffers — zero GC in hot path
    private byte[] _compressBuf;
    private byte[] _decompressBuf;

    void Awake()
    {
        // Load pre-trained dictionary from StreamingAssets
        string dictPath = System.IO.Path.Combine(
            Application.streamingAssetsPath, "game_traffic.dict");
        byte[] blob = System.IO.File.ReadAllBytes(dictPath);
        _dict = NetcDict.Load(blob);

        // Create encoder/decoder contexts
        // DELTA (0x08) | COMPACT_HDR (0x10) | STATEFUL added automatically
        _encCtx = NetcContext.Create(_dict, NetcMode.Stateful, level: 5,
            extraFlags: 0x08 | 0x10);
        _decCtx = NetcContext.Create(_dict, NetcMode.Stateful, level: 5,
            extraFlags: 0x08 | 0x10);

        _compressBuf = new byte[NetcContext.MaxCompressedSize(65535)];
        _decompressBuf = new byte[65535];
    }

    /// <summary>
    /// Compress a packet before sending. Returns compressed bytes.
    /// Call this from your transport layer's send path.
    /// </summary>
    public System.ArraySegment<byte> CompressPacket(byte[] data, int offset, int count)
    {
        int written = _encCtx.Compress(
            new System.ReadOnlySpan<byte>(data, offset, count),
            _compressBuf);
        return new System.ArraySegment<byte>(_compressBuf, 0, written);
    }

    /// <summary>
    /// Decompress a received packet. Returns decompressed bytes.
    /// Call this from your transport layer's receive path.
    /// </summary>
    public System.ArraySegment<byte> DecompressPacket(byte[] data, int offset, int count)
    {
        int written = _decCtx.Decompress(
            new System.ReadOnlySpan<byte>(data, offset, count),
            _decompressBuf);
        return new System.ArraySegment<byte>(_decompressBuf, 0, written);
    }

    /// <summary>
    /// Reset compression state (e.g., on reconnect or scene change).
    /// </summary>
    public void ResetState()
    {
        _encCtx.Reset();
        _decCtx.Reset();
    }

    void OnDestroy()
    {
        _encCtx?.Dispose();
        _decCtx?.Dispose();
        _dict?.Dispose();
    }
}
```

### 5. Training a Dictionary from Unity

You can train a dictionary offline from captured game traffic:

```csharp
// Editor script or standalone tool
using var trainer = new NetcTrainer();

// Capture packets during a play session
foreach (byte[] packet in recordedPackets)
    trainer.AddPacket(packet);

Debug.Log($"Training from {trainer.PacketCount} packets...");
using var dict = trainer.Train(modelId: 1);

// Save to StreamingAssets
byte[] blob = dict.Save();
System.IO.File.WriteAllBytes(
    Application.streamingAssetsPath + "/game_traffic.dict", blob);
Debug.Log("Dictionary saved.");
```

---

## API Reference

All types are in the `Netc` namespace.

### `NetcDict`

`IDisposable` dictionary wrapper. Thread-safe for concurrent reads.

```csharp
byte[] blob = File.ReadAllBytes("trained.dict");
using var dict = NetcDict.Load(blob);

byte[] saved = dict.Save();
byte modelId = dict.ModelId;       // 1-254
```

### `NetcContext`

`IDisposable` compression context. NOT thread-safe — use one per connection per thread.

```csharp
using var ctx = NetcContext.Create(dict, NetcMode.Stateful, level: 5,
    extraFlags: 0x08 | 0x10);

// Compress / Decompress (zero-GC Span API)
int written = ctx.Compress(src, dst);
int recovered = ctx.Decompress(compressed, output);

// Stateless (static methods)
int len = NetcContext.CompressStateless(dict, src, dst);
int len2 = NetcContext.DecompressStateless(dict, compressed, output);

// Utilities
ctx.Reset();
byte simd = ctx.SimdLevel;            // 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON
NetcStats stats = ctx.GetStats();
double ratio = stats.AverageRatio;
```

### `NetcTrainer`

```csharp
using var trainer = new NetcTrainer();
trainer.AddPacket(packetBytes);
using var dict = trainer.Train(modelId: 1);
trainer.Reset();
```

### `NetcException`

```csharp
try { ctx.Compress(src, dst); }
catch (NetcException ex) { Debug.LogError($"{ex.ErrorCode}: {ex.Message}"); }
```

### Enums

**`NetcResult`**: `Ok(0)`, `ErrInvalidArg(-1)`, `ErrBufferTooSmall(-2)`, `ErrCorrupt(-3)`, `ErrModelMismatch(-4)`, `ErrNoMem(-5)`, `ErrDictRequired(-6)`, `ErrNotReady(-7)`

**`NetcMode`**: `Stateful(0)` (TCP), `Stateless(1)` (UDP)

---

## File Structure

```
sdk/csharp/
├── Netc.Core.sln
├── Netc.Core/
│   ├── Netc.Core.csproj
│   ├── NetcNative.cs          # P/Invoke declarations (22 functions)
│   ├── NetcResult.cs
│   ├── NetcMode.cs
│   ├── NetcException.cs
│   ├── NetcStats.cs
│   ├── NetcDict.cs
│   ├── NetcContext.cs
│   └── NetcTrainer.cs
├── tests/
│   └── Netc.Core.Tests/
│       ├── Netc.Core.Tests.csproj
│       ├── GlobalUsings.cs
│       ├── Helpers.cs
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
