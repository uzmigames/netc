# Spec: GDExtension SDK — Godot 4

## ADDED Requirements

### Requirement: Native GDExtension Integration
All netc classes SHALL be registered as proper Godot classes via GDExtension ClassDB, usable from GDScript, C#, and VisualScript.

#### Scenario: GDScript usage
Given a GDScript that creates a NetcContext via `NetcContext.new()`
When `compress(data: PackedByteArray)` is called
Then it SHALL return a PackedByteArray with compressed data
And no engine crash or error SHALL occur

#### Scenario: Editor integration
Given the netc addon is installed in a Godot project
When the editor loads the project
Then NetcDict, NetcContext, and NetcMultiplayerPeer SHALL appear in the class reference
And autocompletion SHALL work in the GDScript editor

### Requirement: RefCounted Lifecycle
NetcDict and NetcContext SHALL extend RefCounted and release native memory when the last reference is dropped.

#### Scenario: Context freed when unreferenced
Given a NetcContext created in a function scope
When the function returns and no other references exist
Then the underlying netc_ctx_t SHALL be freed on the next GC cycle
And no native memory leak SHALL occur

### Requirement: MultiplayerPeer Transparent Compression
NetcMultiplayerPeer SHALL implement Godot's MultiplayerPeerExtension and compress all packets transparently.

#### Scenario: ENet RPC round-trip
Given a Godot scene with two nodes using NetcMultiplayerPeer wrapping ENetMultiplayerPeer
When an RPC is called with a Variant payload
Then the packet SHALL be compressed by the sending peer
And decompressed by the receiving peer
And the Variant value SHALL be identical on both sides

#### Scenario: Fallback on incompressible data
Given a packet containing random bytes
When the peer compresses it
Then the passthrough flag SHALL be set (netc guarantee)
And the received payload SHALL be identical to the original

### Requirement: Cross-Platform Export
The GDExtension SHALL support export to all major Godot targets.

#### Scenario: Android arm64 export
Given a Godot project using NetcMultiplayerPeer
When exported to Android (arm64-v8a)
Then the native library SHALL load correctly
And compression SHALL function identically to desktop

#### Scenario: Web (HTML5) export
Given a Godot project using NetcMultiplayerPeer
When exported to Web via Emscripten
Then the WASM build of libnetc SHALL be included
And compression SHALL function correctly in the browser
