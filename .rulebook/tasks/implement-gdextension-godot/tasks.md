## 1. GDExtension C++ Wrapper
- [ ] 1.1 Set up godot-cpp submodule and SCons/CMake build for GDExtension
- [ ] 1.2 Implement NetcDict (RefCounted, load/save PackedByteArray, train from Array[PackedByteArray])
- [ ] 1.3 Implement NetcContext (RefCounted, stateful/stateless, compress/decompress PackedByteArray API)
- [ ] 1.4 Implement NetcTrainer (train from Array of PackedByteArray, export dict)
- [ ] 1.5 Register all classes and methods with GDCLASS/ClassDB macros
- [ ] 1.6 Write unit tests (GDScript + C++ doctest): round-trip all workloads, edge cases

## 2. Godot MultiplayerPeer Adapter
- [ ] 2.1 Implement NetcMultiplayerPeer (extends MultiplayerPeerExtension)
- [ ] 2.2 Wrap ENetMultiplayerPeer with transparent netc compression/decompression
- [ ] 2.3 Wrap WebRTCMultiplayerPeer with transparent compression
- [ ] 2.4 Write GDScript test scene: server + client, verify packets compress/decompress correctly
- [ ] 2.5 Write Godot export presets for all target platforms

## 3. Native Binary Packaging
- [ ] 3.1 Build targets: Win64, Linux x86_64, macOS ARM64 (universal binary)
- [ ] 3.2 Android NDK build: arm64-v8a (with NEON), x86_64
- [ ] 3.3 iOS static library: arm64
- [ ] 3.4 Web (Emscripten) build for HTML5 export
- [ ] 3.5 Write netc.gdextension manifest with all platform library paths
- [ ] 3.6 CI: build all binaries and publish as GitHub Release artifacts

## 4. Distribution
- [ ] 4.1 Structure sdk/godot/ as a Godot addon (addons/netc/)
- [ ] 4.2 Write plugin.cfg for Godot Asset Library
- [ ] 4.3 Write Godot Asset Library submission metadata

## 5. Documentation
- [ ] 5.1 Write docs/sdk-godot.md (GDExtension internals, GDScript quick start, export guide)
- [ ] 5.2 Write sdk/godot/README.md (install from Asset Library, manual install, usage)
- [ ] 5.3 Update root README.md SDK section with Godot examples
