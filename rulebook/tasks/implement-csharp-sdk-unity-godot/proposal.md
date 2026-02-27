# Proposal: C# SDK — Unity and Godot 4 Integration

## Why

Unity and Godot 4 are the two dominant C# game engines for indie and mid-tier multiplayer games. Both use transport abstractions (Mirror, FishNet, Netcode for GameObjects in Unity; ENetMultiplayerPeer, WebRTCMultiplayerPeer in Godot) that process millions of packets per session. The C core library is directly callable from C# via P/Invoke, but doing so safely requires pinned Span<byte> operations, correct DllImport declarations, and zero-GC-pressure wrappers. Without an official C# SDK, developers risk GC pauses from improper interop patterns — exactly the opposite of what netc is designed to prevent.

## What Changes

- P/Invoke declarations for all public netc functions (`sdk/csharp/Interop/NetcNative.cs`)
- `NetcDict` — IDisposable, thread-safe reads, train/load/save from `ReadOnlySpan<byte>`
- `NetcContext` — IDisposable, TCP and UDP modes, `Compress(ReadOnlySpan<byte>, Span<byte>)` API with no heap allocation
- `NetcTrainer` — dictionary training from `IEnumerable<ReadOnlyMemory<byte>>`
- Unity adapter: `NetcTransport` implementing Mirror `Transport` and FishNet `Transport` abstract classes
- Godot 4 adapter: `NetcMultiplayerPeer` implementing Godot's `MultiplayerPeerExtension`
- NuGet package descriptor and Unity Package Manager (UPM) `package.json`
- Native binary packaging: pre-built `libnetc` for Win64, Linux x86_64, macOS ARM64, Android arm64-v8a, iOS arm64

## Impact

- Affected specs: sdk-csharp/spec.md (new)
- Affected code: sdk/csharp/ (new), docs/sdk-csharp.md (new)
- Breaking change: NO (new module, additive)
- User benefit: Unity and Godot developers can drop netc into their transport layer with 10 lines of code; zero GC pressure in the hot path; works on mobile (Android/iOS) and WebGL (via IL2CPP)
