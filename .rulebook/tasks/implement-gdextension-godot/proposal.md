# Proposal: GDExtension SDK — Godot 4 Integration

## Why

Godot 4 uses GDExtension (C/C++) as its native plugin system, not C#. While Godot has optional C# support via the Mono/.NET build, the standard and most portable integration path is GDExtension. This provides direct C++ access to the engine's `MultiplayerPeerExtension` class, avoids P/Invoke overhead, and works on all Godot export targets including Web, mobile, and consoles. Since netc is a C library, wrapping it in a GDExtension is natural and avoids an unnecessary C → C# → C++ indirection layer.

## What Changes

- GDExtension C++ wrapper for all public netc functions (`sdk/godot/src/`)
- `NetcDict` — RefCounted resource, load/save from Godot `PackedByteArray`
- `NetcContext` — RefCounted, stateful/stateless modes, `compress(PackedByteArray) -> PackedByteArray`
- `NetcMultiplayerPeer` — extends `MultiplayerPeerExtension`, wraps ENet/WebRTC with transparent compression
- GDExtension manifest (`sdk/godot/netc.gdextension`) with platform bindings
- Pre-built native binaries: Win64, Linux x86_64, macOS ARM64, Android arm64-v8a, iOS arm64, Web (Emscripten)
- Godot Asset Library metadata for distribution

## Impact

- Affected specs: sdk-gdextension/spec.md (new)
- Affected code: sdk/godot/ (new), docs/sdk-godot.md (new)
- Breaking change: NO (new module, additive)
- User benefit: Godot developers can add netc to their project as an addon, wrap their existing MultiplayerPeer with compression in 5 lines of GDScript. Works on all export targets without Mono/.NET dependency.
