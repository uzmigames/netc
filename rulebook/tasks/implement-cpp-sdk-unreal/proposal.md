# Proposal: C++ SDK — Unreal Engine 5 Integration

## Why

Unreal Engine 5 is the dominant C++ game engine for high-fidelity multiplayer games. UE5's replication system generates millions of packets per second on dedicated servers. The C core API is a perfect compression layer for this, but UE5 developers need idiomatic C++ wrappers — RAII handles, UE5 memory allocator compatibility, `TSharedPtr`-safe dictionary sharing, and a `.uplugin` descriptor for seamless integration. Without an official C++ SDK, UE5 developers must write their own unsafe P/Invoke-style wrappers, creating a barrier to adoption.

## What Changes

- `sdk/cpp/include/NetcDict.h` — `FNetcDict` RAII wrapper around `netc_dict_t*`, `TSharedRef`-safe, load from file or blob
- `sdk/cpp/include/NetcContext.h` — `FNetcContext` RAII wrapper, TCP and UDP modes, `Compress(TArrayView<uint8>, TArray<uint8>&)` API
- `sdk/cpp/include/NetcTrainer.h` — `FNetcTrainer` for dictionary training from `TArray<TArray<uint8>>`
- `sdk/cpp/NetcPlugin.uplugin` — UE5 plugin descriptor with ThirdParty module for libnetc.a
- `sdk/cpp/NetcPlugin.Build.cs` — UBT build rules linking libnetc, platform detection (Win64, Linux, Mac)
- Full test suite using UE5 Automation Testing framework

## Impact

- Affected specs: sdk-cpp/spec.md (new)
- Affected code: sdk/cpp/ (new), docs/specs/sdk-cpp.md (new)
- Breaking change: NO (new module, additive)
- User benefit: UE5 developers can integrate netc in 5 minutes via plugin; no unsafe C interop required; compatible with UE5 replication, dedicated server, and listen server configurations
