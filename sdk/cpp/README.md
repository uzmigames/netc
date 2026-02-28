# netc C++ SDK

Standalone C++17 RAII wrappers for the netc compression library. Works with any C++ project, including Unreal Engine 5 and Godot 4 (GDExtension).

## Requirements

- C++17 compiler (MSVC 2019+, GCC 8+, Clang 7+, Apple Clang 12+)
- netc C library (static or shared)
- CMake 3.20+ (for building from source)

## Build from Source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_CPP_SDK=ON
cmake --build build --config Release
```

This produces `netc_cpp` (static library) linked against `netc` (static).

### Run Tests

```bash
ctest --test-dir build -C Release --output-on-failure -R test_cpp_sdk
```

47 tests covering Dict, Context, Compress/Decompress, Trainer, RAII safety, and error paths.

---

## Quick Start

```cpp
#include <netc.hpp>

// 1. Train a dictionary from captured packets
netc::Trainer trainer;
for (auto& pkt : captured_packets)
    trainer.AddPacket(pkt.data(), pkt.size());

netc::Dict dict;
netc::Result r = trainer.Train(1, dict);

// 2. Save dictionary to disk for reuse
dict.SaveToFile("game_traffic.dict");

// 3. Compress (TCP stateful, delta + compact headers)
netc::Context ctx(dict, netc::Mode::TCP, /*level=*/5,
    NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

std::vector<uint8_t> compressed;
ctx.Compress(packet.data(), packet.size(), compressed);
// send compressed over network...

// 4. Decompress on the other side
std::vector<uint8_t> recovered;
ctx.Decompress(compressed.data(), compressed.size(), recovered);
```

---

## Integration: Unreal Engine 5

### 1. Build the Libraries

Build netc as static libraries for your target platform:

```bash
# Windows x64
cmake -B build-win64 -G "Visual Studio 17 2022" -A x64 -DNETC_BUILD_CPP_SDK=ON
cmake --build build-win64 --config Release

# Linux x64 (dedicated server)
cmake -B build-linux -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_CPP_SDK=ON
cmake --build build-linux
```

Output files:
- `netc.lib` / `libnetc.a` (C core)
- `netc_cpp.lib` / `libnetc_cpp.a` (C++ SDK)

### 2. Add to Your UE5 Project

Copy libraries and headers into your project:

```
MyProject/
├── Source/
│   └── ThirdParty/
│       └── netc/
│           ├── include/          # Copy sdk/cpp/include/ + include/netc.h
│           │   ├── netc.h
│           │   ├── netc.hpp
│           │   └── netc/
│           │       ├── Dict.hpp
│           │       ├── Context.hpp
│           │       ├── Trainer.hpp
│           │       └── Result.hpp
│           └── lib/
│               ├── Win64/
│               │   ├── netc.lib
│               │   └── netc_cpp.lib
│               └── Linux/
│                   ├── libnetc.a
│                   └── libnetc_cpp.a
```

### 3. Update Your .Build.cs

```csharp
// MyProject.Build.cs
using UnrealBuildTool;
using System.IO;

public class MyProject : ModuleRules
{
    public MyProject(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        string NetcPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "netc");
        string NetcInclude = Path.Combine(NetcPath, "include");
        string NetcLib = Path.Combine(NetcPath, "lib");

        PublicIncludePaths.Add(NetcInclude);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(Path.Combine(NetcLib, "Win64", "netc.lib"));
            PublicAdditionalLibraries.Add(Path.Combine(NetcLib, "Win64", "netc_cpp.lib"));
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(Path.Combine(NetcLib, "Linux", "libnetc.a"));
            PublicAdditionalLibraries.Add(Path.Combine(NetcLib, "Linux", "libnetc_cpp.a"));
        }
    }
}
```

### 4. Usage in UE5 Code

```cpp
// NetcSubsystem.h
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include <netc.hpp>
#include "NetcSubsystem.generated.h"

UCLASS()
class UNetcSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    TArray<uint8> CompressPacket(const TArray<uint8>& Packet);
    TArray<uint8> DecompressPacket(const TArray<uint8>& Packet);

private:
    TUniquePtr<netc::Dict> Dict;
    TUniquePtr<netc::Context> EncCtx;
    TUniquePtr<netc::Context> DecCtx;
};

// NetcSubsystem.cpp
#include "NetcSubsystem.h"

void UNetcSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Load pre-trained dictionary from Content/Data/
    FString DictPath = FPaths::ProjectContentDir() / TEXT("Data/game_traffic.dict");
    Dict = MakeUnique<netc::Dict>();
    netc::Dict::LoadFromFile(TCHAR_TO_UTF8(*DictPath), *Dict);

    uint32 Flags = NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR;
    EncCtx = MakeUnique<netc::Context>(*Dict, netc::Mode::TCP, 5, Flags);
    DecCtx = MakeUnique<netc::Context>(*Dict, netc::Mode::TCP, 5, Flags);
}

void UNetcSubsystem::Deinitialize()
{
    EncCtx.Reset();
    DecCtx.Reset();
    Dict.Reset();
    Super::Deinitialize();
}

TArray<uint8> UNetcSubsystem::CompressPacket(const TArray<uint8>& Packet)
{
    std::vector<uint8_t> Out;
    netc::Result R = EncCtx->Compress(Packet.GetData(), Packet.Num(), Out);
    if (R != netc::Result::OK) return Packet; // fallback: send uncompressed

    TArray<uint8> Result;
    Result.Append(Out.data(), Out.size());
    return Result;
}

TArray<uint8> UNetcSubsystem::DecompressPacket(const TArray<uint8>& Packet)
{
    std::vector<uint8_t> Out;
    netc::Result R = DecCtx->Decompress(Packet.GetData(), Packet.Num(), Out);
    if (R != netc::Result::OK) return Packet;

    TArray<uint8> Result;
    Result.Append(Out.data(), Out.size());
    return Result;
}
```

---

## Integration: Godot 4 (GDExtension)

Godot 4 uses C++ via the GDExtension system. netc integrates as a native GDExtension module.

### 1. Prerequisites

- [godot-cpp](https://github.com/godotengine/godot-cpp) (Godot C++ bindings)
- netc built as static libraries (same steps as UE5 above)

### 2. Project Structure

```
my_gdextension/
├── godot-cpp/                  # git submodule
├── netc/
│   ├── include/                # Copy sdk/cpp/include/ + include/netc.h
│   └── lib/
│       ├── windows.x86_64/
│       │   ├── netc.lib
│       │   └── netc_cpp.lib
│       └── linux.x86_64/
│           ├── libnetc.a
│           └── libnetc_cpp.a
├── src/
│   ├── register_types.h
│   ├── register_types.cpp
│   └── netc_compressor.h
│   └── netc_compressor.cpp
├── my_extension.gdextension
└── SConstruct
```

### 3. SConstruct (SCons Build)

```python
#!/usr/bin/env python
import os

env = SConscript("godot-cpp/SConstruct")

env.Append(CPPPATH=["src/", "netc/include/"])

if env["platform"] == "windows":
    env.Append(LIBPATH=["netc/lib/windows.x86_64/"])
    env.Append(LIBS=["netc_cpp", "netc"])
elif env["platform"] == "linux":
    env.Append(LIBPATH=["netc/lib/linux.x86_64/"])
    env.Append(LIBS=["netc_cpp", "netc"])

sources = Glob("src/*.cpp")
library = env.SharedLibrary("bin/my_extension{}".format(env["SHLIBSUFFIX"]), source=sources)
Default(library)
```

### 4. GDExtension Wrapper Class

```cpp
// src/netc_compressor.h
#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <netc.hpp>

class NetcCompressor : public godot::RefCounted {
    GDCLASS(NetcCompressor, godot::RefCounted)

public:
    NetcCompressor();
    ~NetcCompressor();

    godot::Error load_dictionary(const godot::String &path);
    godot::PackedByteArray compress_packet(const godot::PackedByteArray &data);
    godot::PackedByteArray decompress_packet(const godot::PackedByteArray &data);
    void reset();

protected:
    static void _bind_methods();

private:
    netc::Dict dict_;
    std::unique_ptr<netc::Context> enc_ctx_;
    std::unique_ptr<netc::Context> dec_ctx_;
};

// src/netc_compressor.cpp
#include "netc_compressor.h"
#include <godot_cpp/core/class_db.hpp>

NetcCompressor::NetcCompressor() = default;
NetcCompressor::~NetcCompressor() = default;

void NetcCompressor::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("load_dictionary", "path"),
                                &NetcCompressor::load_dictionary);
    godot::ClassDB::bind_method(godot::D_METHOD("compress_packet", "data"),
                                &NetcCompressor::compress_packet);
    godot::ClassDB::bind_method(godot::D_METHOD("decompress_packet", "data"),
                                &NetcCompressor::decompress_packet);
    godot::ClassDB::bind_method(godot::D_METHOD("reset"),
                                &NetcCompressor::reset);
}

godot::Error NetcCompressor::load_dictionary(const godot::String &path) {
    netc::Result r = netc::Dict::LoadFromFile(path.utf8().get_data(), dict_);
    if (r != netc::Result::OK) return godot::ERR_FILE_CORRUPT;

    uint32_t flags = NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR;
    enc_ctx_ = std::make_unique<netc::Context>(dict_, netc::Mode::TCP, 5, flags);
    dec_ctx_ = std::make_unique<netc::Context>(dict_, netc::Mode::TCP, 5, flags);
    return godot::OK;
}

godot::PackedByteArray NetcCompressor::compress_packet(const godot::PackedByteArray &data) {
    if (!enc_ctx_) return data;

    std::vector<uint8_t> out;
    netc::Result r = enc_ctx_->Compress(data.ptr(), data.size(), out);
    if (r != netc::Result::OK) return data;

    godot::PackedByteArray result;
    result.resize(out.size());
    memcpy(result.ptrw(), out.data(), out.size());
    return result;
}

godot::PackedByteArray NetcCompressor::decompress_packet(const godot::PackedByteArray &data) {
    if (!dec_ctx_) return data;

    std::vector<uint8_t> out;
    netc::Result r = dec_ctx_->Decompress(data.ptr(), data.size(), out);
    if (r != netc::Result::OK) return data;

    godot::PackedByteArray result;
    result.resize(out.size());
    memcpy(result.ptrw(), out.data(), out.size());
    return result;
}

void NetcCompressor::reset() {
    if (enc_ctx_) enc_ctx_->Reset();
    if (dec_ctx_) dec_ctx_->Reset();
}
```

### 5. Usage in GDScript

```gdscript
# Load the extension
var compressor = NetcCompressor.new()
compressor.load_dictionary("res://data/game_traffic.dict")

# In your network code
func _send_packet(data: PackedByteArray):
    var compressed = compressor.compress_packet(data)
    peer.put_packet(compressed)

func _on_packet_received(data: PackedByteArray):
    var decompressed = compressor.decompress_packet(data)
    _process_game_state(decompressed)
```

### 6. .gdextension File

```ini
[configuration]
entry_symbol = "my_extension_init"
compatibility_minimum = "4.2"

[libraries]
windows.release.x86_64 = "res://bin/my_extension.dll"
linux.release.x86_64 = "res://bin/libmy_extension.so"
macos.release = "res://bin/libmy_extension.dylib"
```

---

## API Reference

All types are in `namespace netc`. Include `<netc.hpp>` for the umbrella header.

### `netc::Dict`

Move-only dictionary wrapper. Thread-safe for concurrent reads.

```cpp
// Load from memory
netc::Dict dict;
netc::Result r = netc::Dict::LoadFromBytes(blob_data, blob_size, dict);

// Load from file
netc::Dict::LoadFromFile("trained.dict", dict);

// Save
std::vector<uint8_t> bytes;
dict.SaveToBytes(bytes);
dict.SaveToFile("output.dict");

// Inspect
uint8_t model_id = dict.GetModelId();  // 1-254
bool valid = dict.IsValid();
```

### `netc::Context`

Move-only compression context. NOT thread-safe — use one per connection per thread.

```cpp
// Create with flags
netc::Context ctx(dict, netc::Mode::TCP, /*level=*/5,
    NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

// Compress / Decompress
std::vector<uint8_t> compressed, recovered;
ctx.Compress(src, src_size, compressed);
ctx.Decompress(compressed.data(), compressed.size(), recovered);

// Stateless (static methods, no context state)
netc::Context::CompressStateless(dict, src, src_size, compressed);
netc::Context::DecompressStateless(dict, compressed.data(), compressed.size(), recovered);

// Utilities
ctx.Reset();
netc::SimdLevel lvl = ctx.GetSimdLevel();
netc::Stats stats;
ctx.GetStats(stats);
double ratio = stats.AverageRatio();
```

### `netc::Trainer`

```cpp
netc::Trainer trainer;
trainer.AddPacket(pkt_data, pkt_size);

std::vector<std::vector<uint8_t>> corpus = { ... };
trainer.AddPackets(corpus);

netc::Dict dict;
trainer.Train(/*model_id=*/1, dict);
trainer.Reset();
```

### `netc::Result`

| Value | Name | Description |
|-------|------|-------------|
| 0 | `OK` | Success |
| -1 | `NoMem` | Memory allocation failed |
| -2 | `TooBig` | Input exceeds maximum size |
| -3 | `Corrupt` | Corrupted or truncated data |
| -4 | `DictInvalid` | Invalid dictionary |
| -5 | `BufSmall` | Output buffer insufficient |
| -6 | `CtxNull` | NULL context |
| -7 | `Unsupported` | Unsupported operation |
| -8 | `Version` | Version mismatch |
| -9 | `InvalidArg` | Invalid argument |

```cpp
const char* msg = netc::ResultToString(r);
```

---

## File Structure

```
sdk/cpp/
├── include/
│   ├── netc.hpp              # Umbrella header
│   └── netc/
│       ├── Result.hpp        # Result enum + ResultToString
│       ├── Dict.hpp          # Dict class
│       ├── Context.hpp       # Context + Mode/SimdLevel/Stats
│       └── Trainer.hpp       # Trainer class
├── src/
│   ├── Dict.cpp
│   ├── Context.cpp
│   └── Trainer.cpp
├── tests/
│   └── test_cpp_sdk.cpp      # 47 unit tests
└── README.md
```

## License

Apache License 2.0 — see [LICENSE](../../LICENSE).
