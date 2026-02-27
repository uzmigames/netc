# netc C++ SDK — Unreal Engine 5

```
SDK Version:  0.1.0
UE5 Support:  5.3+
Platforms:    Win64, Linux x86_64, macOS ARM64
Compiler:     MSVC 2022, GCC 12+, Clang 15+
Depends:      netc core library (libnetc)
```

---

## Table of Contents

1. [Overview](#1-overview)
2. [Installation](#2-installation)
3. [Core Concepts](#3-core-concepts)
4. [API Reference](#4-api-reference)
   - [FNetcDict](#41-fnetcdict)
   - [FNetcContext](#42-fnetccontext)
   - [FNetcTrainer](#43-fnetctrainer)
   - [ENetcResult](#44-enetcresult)
   - [ENetcMode](#45-enetcmode)
5. [Quick Start](#5-quick-start)
6. [Dictionary Workflow](#6-dictionary-workflow)
7. [TCP Stateful Compression](#7-tcp-stateful-compression)
8. [UDP Stateless Compression](#8-udp-stateless-compression)
9. [UE5 Integration Patterns](#9-ue5-integration-patterns)
10. [Thread Safety](#10-thread-safety)
11. [Memory Model](#11-memory-model)
12. [SIMD and Platform Notes](#12-simd-and-platform-notes)
13. [Error Handling](#13-error-handling)
14. [Build System](#14-build-system)
15. [Testing](#15-testing)

---

## 1. Overview

The netc C++ SDK wraps the netc C core library (`libnetc`) with idiomatic Unreal Engine 5 types:

- **RAII handles** — `FNetcDict` and `FNetcContext` own their underlying resources and release them on destruction. No manual `Free()` calls required.
- **UE5-native API** — accepts `TArrayView<const uint8>`, `TArray<uint8>&`, `FString`, and `IPlatformFile` references.
- **`TSharedRef`-safe dictionaries** — a single trained dictionary can be shared read-only across all connections on all threads without locks.
- **Zero hot-path allocation** — `Compress` and `Decompress` write into caller-provided `TArray` buffers. No internal heap activity per packet.
- **Move semantics** — contexts are movable but not copyable, consistent with UE5 socket ownership patterns.

### What the SDK does NOT do

- Manage sockets or connections (use `FSocket`, `INetworkingWebSocket`, or the Iris/NetDriver layer)
- Implement reliable delivery
- Encrypt data

---

## 2. Installation

### 2.1 As a UE5 Plugin

1. Copy the `sdk/cpp/` directory into your project's `Plugins/Netc/` folder:

```
MyGame/
└── Plugins/
    └── Netc/
        ├── NetcPlugin.uplugin
        ├── Source/
        │   └── NetcPlugin/
        │       ├── NetcPlugin.Build.cs
        │       ├── Public/
        │       │   ├── NetcDict.h
        │       │   ├── NetcContext.h
        │       │   └── NetcTrainer.h
        │       └── Private/
        │           └── NetcPlugin.cpp
        └── ThirdParty/
            └── netc/
                ├── include/
                │   └── netc.h
                └── lib/
                    ├── Win64/
                    │   └── netc.lib
                    ├── Linux/
                    │   └── libnetc.a
                    └── Mac/
                        └── libnetc.a
```

2. Add the plugin to your `.uproject` file:

```json
{
  "Plugins": [
    {
      "Name": "Netc",
      "Enabled": true
    }
  ]
}
```

3. Add `"Netc"` to your module's `PublicDependencyModuleNames` in `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "Core", "CoreUObject", "Engine", "Netc"
});
```

4. Regenerate project files and build.

### 2.2 As a Standalone C++ Library

If you are not using UE5, include the headers directly and link `libnetc`:

```cmake
target_include_directories(MyTarget PRIVATE netc/sdk/cpp/include)
target_link_libraries(MyTarget PRIVATE netc)
```

The SDK headers have a `NETC_UE5` guard — when `UE_BUILD_*` macros are not defined, they fall back to `std::vector<uint8_t>` and `std::span` equivalents.

---

## 3. Core Concepts

### Dictionary

A dictionary is a frozen statistical model trained on representative packets from your protocol. It is **immutable** after training and **read-only** during compression. One dictionary serves all connections in a session.

```
Training corpus (50,000 packets)
         │
         ▼
   FNetcTrainer::Train()
         │
         ▼
     FNetcDict
    (read-only)
    /    |    \
   /     |     \
Ctx1   Ctx2   Ctx3   ← one context per connection, shared dict
```

### Context

A context (`FNetcContext`) is the per-connection compression state. It holds:
- A reference to the shared `FNetcDict`
- A ring buffer of recent packet history (TCP mode)
- The current ANS encoder state
- A pre-allocated scratch arena (no malloc in hot path)

Contexts are **not thread-safe**. Each connection (thread) must have its own.

### Mode

| Mode | State | Use case |
|------|-------|----------|
| `ENetcMode::TCP` | Stateful — ring buffer accumulates history | Reliable ordered connections |
| `ENetcMode::UDP` | Stateless — each packet self-contained | Unreliable datagrams, multicast |

---

## 4. API Reference

### 4.1 FNetcDict

```cpp
class NETCPLUGIN_API FNetcDict final
{
public:
    // ─── Construction ───────────────────────────────────────────────

    /** Default-constructs an empty (invalid) dictionary. */
    FNetcDict() = default;

    /** Move-only. Dictionaries cannot be copied. */
    FNetcDict(FNetcDict&&) noexcept;
    FNetcDict& operator=(FNetcDict&&) noexcept;
    FNetcDict(const FNetcDict&) = delete;
    FNetcDict& operator=(const FNetcDict&) = delete;

    ~FNetcDict();

    // ─── Factory methods ─────────────────────────────────────────────

    /**
     * Load a dictionary from a binary blob (e.g., embedded asset, network blob).
     *
     * @param Data      Pointer to the binary dictionary blob.
     * @param Size      Size of the blob in bytes.
     * @param OutDict   Receives the loaded dictionary on success.
     * @return          ENetcResult::OK on success.
     *                  ENetcResult::DictInvalid if checksum fails.
     *                  ENetcResult::Version if format version mismatch.
     */
    static ENetcResult LoadFromBytes(
        const uint8* Data, int32 Size, FNetcDict& OutDict);

    /**
     * Load a dictionary from an absolute file path.
     *
     * @param FilePath  Absolute path to the .netcdict file.
     * @param OutDict   Receives the loaded dictionary on success.
     * @return          ENetcResult::OK on success.
     */
    static ENetcResult LoadFromFile(
        const FString& FilePath, FNetcDict& OutDict);

    // ─── Serialization ───────────────────────────────────────────────

    /**
     * Serialize the dictionary to a binary blob.
     *
     * @param OutBytes  Receives the serialized bytes.
     * @return          ENetcResult::OK on success.
     */
    ENetcResult SaveToBytes(TArray<uint8>& OutBytes) const;

    /**
     * Save the dictionary to a file.
     *
     * @param FilePath  Absolute path for the output file.
     * @return          ENetcResult::OK on success.
     */
    ENetcResult SaveToFile(const FString& FilePath) const;

    // ─── Inspection ──────────────────────────────────────────────────

    /** Returns true if the dictionary holds a valid trained model. */
    bool IsValid() const;

    /** Number of packets the dictionary was trained on. */
    int32 GetCorpusSize() const;

    /** Size of the dictionary in bytes. */
    int32 GetDictSize() const;

    // ─── Internal ────────────────────────────────────────────────────
    const netc_dict_t* GetNativeDict() const { return NativeDict; }

private:
    netc_dict_t* NativeDict = nullptr;
};
```

---

### 4.2 FNetcContext

```cpp
class NETCPLUGIN_API FNetcContext final
{
public:
    // ─── Construction ───────────────────────────────────────────────

    /**
     * Create a compression context.
     *
     * @param Dict      A valid FNetcDict. Must outlive this context.
     *                  Can be shared across multiple contexts (read-only).
     * @param Mode      ENetcMode::TCP (stateful) or ENetcMode::UDP (stateless).
     * @param Level     Compression level 0–9 (default: 5).
     *                  0 = fastest, 9 = best ratio.
     */
    explicit FNetcContext(
        const FNetcDict& Dict,
        ENetcMode        Mode  = ENetcMode::TCP,
        uint8            Level = 5);

    /** Move-only. Contexts cannot be copied. */
    FNetcContext(FNetcContext&&) noexcept;
    FNetcContext& operator=(FNetcContext&&) noexcept;
    FNetcContext(const FNetcContext&) = delete;
    FNetcContext& operator=(const FNetcContext&) = delete;

    ~FNetcContext();

    // ─── Compression ─────────────────────────────────────────────────

    /**
     * Compress a single packet.
     *
     * Hot path — zero internal heap allocations when Dst is pre-allocated.
     * Call MaxCompressedSize() to determine the required Dst capacity.
     *
     * @param Src       Input packet bytes.
     * @param Dst       Output buffer. Will be resized to actual compressed size.
     *                  Pre-allocate with Dst.Reserve(MaxCompressedSize(Src.Num()))
     *                  to avoid allocation in this call.
     * @return          ENetcResult::OK on success.
     *                  ENetcResult::TooBig if Src exceeds NETC_MAX_PACKET_SIZE.
     *                  ENetcResult::BufSmall if Dst capacity is insufficient.
     */
    ENetcResult Compress(
        TArrayView<const uint8> Src,
        TArray<uint8>&          Dst);

    /**
     * Decompress a single packet.
     *
     * @param Src       Compressed bytes produced by Compress().
     * @param Dst       Output buffer. Will be resized to decompressed size.
     * @return          ENetcResult::OK on success.
     *                  ENetcResult::Corrupt if the bitstream is invalid.
     *                  ENetcResult::DictInvalid if dict checksum mismatch.
     */
    ENetcResult Decompress(
        TArrayView<const uint8> Src,
        TArray<uint8>&          Dst);

    // ─── UDP Stateless (no context state used) ───────────────────────

    /**
     * Compress without updating context state.
     * Suitable for UDP datagrams where packet order is not guaranteed.
     */
    ENetcResult CompressStateless(
        TArrayView<const uint8> Src,
        TArray<uint8>&          Dst);

    /** Decompress without updating context state. */
    ENetcResult DecompressStateless(
        TArrayView<const uint8> Src,
        TArray<uint8>&          Dst);

    // ─── Utilities ───────────────────────────────────────────────────

    /**
     * Returns the maximum possible compressed size for an input of SrcSize bytes.
     * Use this to pre-allocate Dst to avoid reallocation in Compress().
     *
     * Formula: SrcSize + NETC_HEADER_SIZE (8 bytes).
     * Passthrough packets are never larger than input + header.
     */
    static int32 MaxCompressedSize(int32 SrcSize);

    /**
     * Reset context state to initial conditions.
     * Call between game sessions or on reconnect.
     * Dictionary is retained; ring buffer is cleared.
     */
    void Reset();

    /** Returns the SIMD acceleration level active for this context. */
    ENetcSimdLevel GetSimdLevel() const;

    /** Returns compression statistics (only populated if NETC_CFG_FLAG_STATS was set). */
    FNetcStats GetStats() const;

    bool IsValid() const { return NativeCtx != nullptr; }

private:
    netc_ctx_t* NativeCtx = nullptr;
};
```

---

### 4.3 FNetcTrainer

```cpp
class NETCPLUGIN_API FNetcTrainer final
{
public:
    FNetcTrainer() = default;

    /**
     * Add a packet to the training corpus.
     * Call this for each representative packet captured from your protocol.
     *
     * @param Packet    Packet bytes to add to the corpus.
     */
    void AddPacket(TArrayView<const uint8> Packet);

    /**
     * Add multiple packets at once.
     *
     * @param Packets   Array of packet byte arrays.
     */
    void AddPackets(const TArray<TArray<uint8>>& Packets);

    /**
     * Returns the number of packets currently in the corpus.
     */
    int32 GetCorpusCount() const;

    /**
     * Train and produce a dictionary from the current corpus.
     *
     * Requires at minimum 1,000 packets (NETC_MIN_CORPUS_SIZE).
     * Recommended: 10,000–50,000 packets from a representative session.
     *
     * This call is NOT on the hot path — training is an offline step.
     * Expected duration: < 500 ms for 10,000 packets.
     *
     * @param OutDict   Receives the trained dictionary on success.
     * @return          ENetcResult::OK on success.
     *                  ENetcResult::NoMem if corpus is below minimum size.
     */
    ENetcResult Train(FNetcDict& OutDict) const;

    /**
     * Clear the corpus (frees all accumulated packet data).
     */
    void Reset();

private:
    TArray<TArray<uint8>> Corpus;
};
```

---

### 4.4 ENetcResult

```cpp
enum class ENetcResult : int32
{
    OK            =  0,   // Success
    NoMem         = -1,   // Internal allocation failure (arena too small)
    TooBig        = -2,   // Input exceeds NETC_MAX_PACKET_SIZE (65535 bytes)
    Corrupt       = -3,   // Decompressed bitstream is corrupt or truncated
    DictInvalid   = -4,   // Dictionary CRC32 checksum mismatch
    BufSmall      = -5,   // Dst buffer capacity insufficient
    CtxNull       = -6,   // Context is invalid (default-constructed or moved-from)
    Unsupported   = -7,   // Algorithm or feature not available on this platform
    Version       = -8,   // Dictionary format version mismatch
};

/** Returns a human-readable description of the result code. */
NETCPLUGIN_API const TCHAR* NetcResultToString(ENetcResult Result);
```

---

### 4.5 ENetcMode

```cpp
enum class ENetcMode : uint8
{
    /** Stateful TCP mode. Context accumulates ring buffer history across packets.
     *  Compress and Decompress must be called in the same order on both sides. */
    TCP = 0,

    /** Stateless UDP mode. Each packet is self-contained.
     *  CompressStateless / DecompressStateless use the dictionary only,
     *  no shared state between packets. */
    UDP = 1,
};
```

---

### 4.6 Supporting Types

```cpp
enum class ENetcSimdLevel : uint8
{
    Generic = 1,  // C fallback (no intrinsics)
    SSE42   = 2,  // x86 SSE4.2
    AVX2    = 3,  // x86 AVX2 (256-bit vectors)
    NEON    = 4,  // ARM NEON (128-bit vectors)
};

struct FNetcStats
{
    uint64 PacketsCompressed;
    uint64 PacketsDecompressed;
    uint64 BytesIn;
    uint64 BytesOut;
    double AverageRatio;      // BytesOut / BytesIn
    double AverageLatencyNs;  // Average per-packet compress time
};
```

---

## 5. Quick Start

```cpp
#include "Netc/NetcDict.h"
#include "Netc/NetcContext.h"

void AMyGameMode::BeginPlay()
{
    // 1. Load a pre-trained dictionary (shipped with game content)
    FNetcDict Dict;
    ENetcResult LoadResult = FNetcDict::LoadFromFile(
        FPaths::ProjectContentDir() / TEXT("Netc/game_packets.netcdict"), Dict);
    checkf(LoadResult == ENetcResult::OK, TEXT("netc: failed to load dict: %s"),
           NetcResultToString(LoadResult));

    // 2. Create one context per player connection
    FNetcContext CompressCtx(Dict, ENetcMode::TCP);
    FNetcContext DecompressCtx(Dict, ENetcMode::TCP);

    // 3. Compress a packet
    TArray<uint8> RawPacket = BuildPlayerStatePacket();
    TArray<uint8> Compressed;
    Compressed.Reserve(FNetcContext::MaxCompressedSize(RawPacket.Num()));

    ENetcResult Result = CompressCtx.Compress(RawPacket, Compressed);
    check(Result == ENetcResult::OK);

    // 4. Send Compressed over the wire...
    SendToClient(Compressed);

    // 5. On the receiver side, decompress
    TArray<uint8> Recovered;
    Result = DecompressCtx.Decompress(Compressed, Recovered);
    check(Result == ENetcResult::OK);
    check(Recovered == RawPacket); // byte-for-byte identical
}
```

---

## 6. Dictionary Workflow

### 6.1 Capturing a Training Corpus

Train the dictionary during development by capturing representative packets:

```cpp
// In a development build, intercept packets before sending
class FNetcCapture
{
public:
    void CapturePacket(TArrayView<const uint8> Packet)
    {
        if (Trainer.GetCorpusCount() < 50000)
        {
            Trainer.AddPacket(Packet);
        }
        else if (!bSaved)
        {
            FNetcDict Dict;
            ENetcResult R = Trainer.Train(Dict);
            check(R == ENetcResult::OK);
            Dict.SaveToFile(FPaths::ProjectSavedDir() / TEXT("netc_corpus.netcdict"));
            bSaved = true;
            UE_LOG(LogNetc, Log, TEXT("netc: dictionary saved (%d packets)"),
                   Trainer.GetCorpusCount());
        }
    }

private:
    FNetcTrainer Trainer;
    bool bSaved = false;
};
```

### 6.2 Shipping a Dictionary

1. Train the dictionary using the captured corpus.
2. Copy the `.netcdict` file to `Content/Netc/`.
3. Mark it as a non-asset raw binary in your `.uproject` packaging rules.
4. Load it at game startup and keep the `FNetcDict` alive for the session lifetime.

### 6.3 Dictionary Lifetime

```cpp
// Recommended pattern: store in GameInstance so it lives for the full session
UCLASS()
class UMyGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override
    {
        Super::Init();
        ENetcResult R = FNetcDict::LoadFromFile(
            FPaths::ProjectContentDir() / TEXT("Netc/packets.netcdict"),
            NetcDict);
        checkf(R == ENetcResult::OK, TEXT("netc dict load failed: %s"),
               NetcResultToString(R));
    }

    // Shared across all connections — thread-safe for concurrent reads
    FNetcDict NetcDict;
};
```

---

## 7. TCP Stateful Compression

In TCP mode, the context accumulates a ring buffer of previous packets. Each new packet is delta-encoded against the history, significantly improving compression ratio on correlated game state streams.

**Critical constraint**: compress and decompress contexts on both sides MUST process packets in the same order. If a packet is lost and retransmitted, both sides must process it (TCP already guarantees this).

```cpp
// Server side — one FNetcContext per client connection
class FClientSession
{
public:
    FClientSession(const FNetcDict& SharedDict)
        : SendCtx(SharedDict, ENetcMode::TCP)
        , RecvCtx(SharedDict, ENetcMode::TCP)
    {}

    // Called per game tick for each client
    void SendPlayerState(const FPlayerState& State)
    {
        // Serialize to bytes
        TArray<uint8> Raw;
        FMemoryWriter Writer(Raw);
        State.Serialize(Writer);

        // Compress — ring buffer updated automatically
        TArray<uint8> Compressed;
        Compressed.Reserve(FNetcContext::MaxCompressedSize(Raw.Num()));
        ENetcResult R = SendCtx.Compress(Raw, Compressed);
        checkf(R == ENetcResult::OK, TEXT("compress failed: %s"), NetcResultToString(R));

        // Send
        ClientSocket->Send(Compressed.GetData(), Compressed.Num(), BytesSent);
    }

    void OnPacketReceived(TArrayView<const uint8> Compressed)
    {
        TArray<uint8> Raw;
        ENetcResult R = RecvCtx.Decompress(Compressed, Raw);
        checkf(R == ENetcResult::OK, TEXT("decompress failed: %s"), NetcResultToString(R));

        FMemoryReader Reader(Raw);
        FPlayerInput Input;
        Input.Serialize(Reader);
        ProcessInput(Input);
    }

    // Call on reconnect — clears ring buffer, keeps dictionary
    void OnReconnect()
    {
        SendCtx.Reset();
        RecvCtx.Reset();
    }

private:
    FNetcContext SendCtx;
    FNetcContext RecvCtx;
};
```

---

## 8. UDP Stateless Compression

In UDP mode, each packet carries enough information to decompress independently. No ring buffer is maintained. Suitable for unreliable fast-path packets (position updates, input, ephemeral game state).

```cpp
// UDP position update — stateless, no shared history
void FUdpPositionChannel::SendPosition(const FVector& Position, uint16 SequenceNum)
{
    // Pack position into bytes
    uint8 Raw[32];
    FMemoryWriter Writer(MakeArrayView(Raw, sizeof(Raw)));
    Writer << SequenceNum;
    Writer << Position;

    // Stateless compress — does not update ring buffer
    TArray<uint8> Compressed;
    ENetcResult R = Ctx.CompressStateless(MakeArrayView(Raw, 32), Compressed);
    check(R == ENetcResult::OK);

    UDPSocket->SendTo(Compressed.GetData(), Compressed.Num(), RemoteAddr);
}

void FUdpPositionChannel::OnDatagramReceived(TArrayView<const uint8> Data)
{
    TArray<uint8> Raw;
    ENetcResult R = Ctx.DecompressStateless(Data, Raw);
    if (R != ENetcResult::OK)
    {
        UE_LOG(LogNetc, Warning, TEXT("UDP decompress failed: %s"), NetcResultToString(R));
        return;
    }

    FMemoryReader Reader(Raw);
    uint16 Seq; FVector Pos;
    Reader << Seq << Pos;
    ApplyPosition(Seq, Pos);
}
```

---

## 9. UE5 Integration Patterns

### 9.1 NetDriver / Channel Integration

Integrate netc transparently at the `UIpNetDriver` level so all UE5 replication traffic is compressed without modifying game code:

```cpp
// In a custom UIpNetDriver subclass
int32 UNetcNetDriver::LowLevelSend(
    TSharedPtr<const FInternetAddr> Address,
    void* Data, int32 CountBits, FOutPacketTraits& Traits)
{
    TArrayView<const uint8> Raw(
        reinterpret_cast<const uint8*>(Data), (CountBits + 7) / 8);

    CompressBuffer.Reset();
    CompressBuffer.Reserve(FNetcContext::MaxCompressedSize(Raw.Num()));

    ENetcResult R = SendCtx.CompressStateless(Raw, CompressBuffer);
    if (R != ENetcResult::OK)
    {
        UE_LOG(LogNetc, Warning, TEXT("Send compress failed: %s"), NetcResultToString(R));
        return Super::LowLevelSend(Address, Data, CountBits, Traits);
    }

    return Super::LowLevelSend(
        Address,
        CompressBuffer.GetData(),
        CompressBuffer.Num() * 8,
        Traits);
}
```

### 9.2 NetworkPrediction Plugin

netc is compatible with UE5's NetworkPrediction plugin. Compress the serialized state blobs before they enter the NetDriver layer — the prediction rollback mechanism is unaffected because it operates above the transport layer.

### 9.3 Iris Replication System (UE5.3+)

For UE5.3+ Iris replication, insert netc as a packet modifier in the `FNetPacketNotify` pipeline. See `sdk/cpp/Source/NetcPlugin/Private/NetcIrisAdapter.cpp` for a reference implementation.

### 9.4 Per-Channel Compression

For fine-grained control, apply compression per UE5 channel type:

```cpp
// Compress only movement and voice channels (highest bandwidth)
// Leave RPC channels uncompressed (low volume, already small)
bool ShouldCompress(EChannelType ChannelType)
{
    return ChannelType == CHTYPE_Voice || ChannelType == CHTYPE_Actor;
}
```

---

## 10. Thread Safety

| Object | Thread Safety |
|--------|---------------|
| `FNetcDict` | **Read-safe**: concurrent reads from any number of threads |
| `FNetcDict` training | **NOT safe**: `FNetcTrainer::Train()` is single-threaded |
| `FNetcContext` | **NOT safe**: one context per thread (one per connection) |
| `FNetcTrainer` | **NOT safe**: single-threaded accumulation |

**Correct multi-threaded pattern:**

```cpp
// GameInstance (main thread): load dict once
FNetcDict SharedDict;  // shared read-only

// Connection thread A
FNetcContext CtxA(SharedDict, ENetcMode::TCP);  // private to thread A

// Connection thread B
FNetcContext CtxB(SharedDict, ENetcMode::TCP);  // private to thread B

// Both threads can compress simultaneously — dict is read-only, safe
```

---

## 11. Memory Model

### 11.1 Per-Context Memory Budget

| Component | Size | Notes |
|-----------|------|-------|
| Ring buffer | 64 KB (default) | Configurable via Level param |
| ANS tables | ~4 KB | Cached in L2 |
| Scratch arena | 3 KB | 2× max packet size |
| Context overhead | < 512 bytes | struct fields |
| **Total per context** | **~68 KB** | Well within UE5 per-thread budget |

### 11.2 Dictionary Memory

| Component | Size |
|-----------|------|
| Byte frequency table | 256 bytes |
| Bigram model (full) | 128 KB |
| **Dictionary total** | **≤ 512 KB** |

The dictionary is shared across all connections — its memory cost is paid once per session.

### 11.3 Hot-Path Allocation Policy

`FNetcContext::Compress` and `Decompress` perform **zero heap allocations** provided the output `TArray` has sufficient capacity. Pre-allocate with:

```cpp
TArray<uint8> Dst;
Dst.Reserve(FNetcContext::MaxCompressedSize(MaxPacketSize));
// Reuse Dst for every packet — it will be SetNum'd, not reallocated
```

---

## 12. SIMD and Platform Notes

| Platform | SIMD | Notes |
|----------|------|-------|
| Win64 (MSVC) | AVX2 | Detected via CPUID, `/arch:AVX2` in Build.cs |
| Win64 (no AVX2) | SSE4.2 | Automatic fallback |
| Linux x86_64 | AVX2 | `-mavx2` in Build.cs |
| macOS ARM64 | NEON | Always available on Apple Silicon |
| Android arm64-v8a | NEON | Enabled via NDK flags |
| Generic / unknown | C fallback | All outputs identical |

SIMD selection happens at `FNetcContext` construction time. Use `GetSimdLevel()` to verify:

```cpp
FNetcContext Ctx(Dict);
UE_LOG(LogNetc, Log, TEXT("netc SIMD: %d"), (int32)Ctx.GetSimdLevel());
// ENetcSimdLevel::AVX2 = 3 on a modern x86 machine
```

The output of all SIMD paths is **byte-for-byte identical**. A packet compressed on an AVX2 server will decompress correctly on a NEON mobile client.

---

## 13. Error Handling

All SDK functions return `ENetcResult`. Use `NetcResultToString()` for logging:

```cpp
ENetcResult R = Ctx.Compress(Src, Dst);
if (R != ENetcResult::OK)
{
    UE_LOG(LogNetc, Error, TEXT("netc compress: %s"), NetcResultToString(R));
    // Handle error — e.g., send uncompressed, disconnect, etc.
    return;
}
```

### Common Errors

| Error | Cause | Resolution |
|-------|-------|------------|
| `DictInvalid` | Dictionary file corrupted or version mismatch | Retrain and redeploy dictionary |
| `Corrupt` | Received packet was truncated or tampered | Drop packet; log and investigate |
| `TooBig` | Input packet > 65535 bytes | Fragment before compressing |
| `BufSmall` | Dst not pre-allocated | Call `MaxCompressedSize()` and reserve |
| `CtxNull` | Context moved-from or default-constructed | Check context validity before use |

### Passthrough Packets

When compressed size ≥ original size, netc activates passthrough automatically — the compressed output contains the original bytes verbatim with a passthrough flag. This is **not an error**. The output is valid and `Decompress` handles it transparently.

---

## 14. Build System

### 14.1 NetcPlugin.Build.cs

```csharp
using System.IO;
using UnrealBuildTool;

public class NetcPlugin : ModuleRules
{
    public NetcPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        // netc C core — ThirdParty static library
        string NetcLibPath = Path.Combine(
            ModuleDirectory, "..", "..", "ThirdParty", "netc");

        PublicIncludePaths.Add(Path.Combine(NetcLibPath, "include"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(
                Path.Combine(NetcLibPath, "lib", "Win64", "netc.lib"));
            PublicDefinitions.Add("NETC_SIMD_AVX2=1");
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Add(
                Path.Combine(NetcLibPath, "lib", "Linux", "libnetc.a"));
            PublicDefinitions.Add("NETC_SIMD_AVX2=1");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            PublicAdditionalLibraries.Add(
                Path.Combine(NetcLibPath, "lib", "Mac", "libnetc.a"));
            PublicDefinitions.Add("NETC_SIMD_NEON=1");
        }

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Networking"
        });
    }
}
```

### 14.2 Building Pre-compiled Libraries

```bash
# From the netc repository root

# Win64 (cross-compile or on Windows)
cmake -B build/win64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="/arch:AVX2" \
      -DNETC_SIMD_LEVEL=AVX2
cmake --build build/win64 --target netc
cp build/win64/libnetc.a sdk/cpp/ThirdParty/netc/lib/Win64/netc.lib

# Linux x86_64
cmake -B build/linux -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-march=x86-64-v3"
cmake --build build/linux --target netc
cp build/linux/libnetc.a sdk/cpp/ThirdParty/netc/lib/Linux/libnetc.a

# macOS ARM64
cmake -B build/mac -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/mac --target netc
cp build/mac/libnetc.a sdk/cpp/ThirdParty/netc/lib/Mac/libnetc.a
```

---

## 15. Testing

### 15.1 Standalone Unit Tests (no UE5 required)

```bash
# Build and run the standalone C++ SDK tests
cmake -B build/test -DNETC_BUILD_SDK_CPP_TESTS=ON
cmake --build build/test --target netc_sdk_cpp_tests
./build/test/netc_sdk_cpp_tests
```

Tests cover:
- `FNetcDict` load/save/train round-trip
- `FNetcContext` compress/decompress on all WL-001..WL-008 workloads
- TCP stateful mode: 10,000 packet sequence, verify all decompress correctly
- UDP stateless mode: out-of-order packets, verify no crash
- Thread safety: 8 threads × 10,000 packets with shared `FNetcDict`
- Move semantics: no double-free
- Error paths: null dict, corrupt packet, oversized input

### 15.2 UE5 Automation Tests

In UE5 Editor, open **Session Frontend → Automation** and filter by `Netc`:

```
Netc.Dict.LoadSaveRoundTrip
Netc.Context.CompressDecompress.TCP
Netc.Context.CompressDecompress.UDP
Netc.Context.ThreadSafety
Netc.Context.Passthrough
Netc.Error.CorruptPacket
Netc.Error.OversizedPacket
Netc.SIMD.OutputConsistency
```

All tests must pass before shipping.

---

*See also: [docs/specs/sdk-csharp.md](sdk-csharp.md) — C# SDK for Unity and Godot 4*
