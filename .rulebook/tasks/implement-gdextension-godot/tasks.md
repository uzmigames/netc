## 1. GDExtension C++ Wrapper
- [ ] 1.1 Set up godot-cpp submodule and SCons/CMake build for GDExtension
- [ ] 1.2 Implement NetcDict (RefCounted, load/save PackedByteArray, train from Array[PackedByteArray])
- [ ] 1.3 Implement NetcContext (RefCounted, stateful/stateless, compress/decompress PackedByteArray API)
- [ ] 1.4 Implement NetcTrainer (train from Array of PackedByteArray, export dict)
- [ ] 1.5 Register all classes and methods with GDCLASS/ClassDB macros
- [ ] 1.6 Write unit tests: round-trip all workloads, edge cases

## 2. Native Binary Packaging
- [ ] 2.1 Build targets: Win64, Linux x86_64, macOS ARM64
- [ ] 2.2 Write netc.gdextension manifest with platform library paths
- [ ] 2.3 CI: build all binaries and publish as GitHub Release artifacts

## 3. Documentation
- [ ] 3.1 Write sdk/godot/README.md (install, GDScript API reference, examples)
- [ ] 3.2 Update root README.md SDK section with Godot examples
