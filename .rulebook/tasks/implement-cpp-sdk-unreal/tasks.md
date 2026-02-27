## 1. Core C++ Wrappers
- [ ] 1.1 Implement FNetcDict (RAII, TSharedRef-compatible, LoadFromFile, LoadFromBytes)
- [ ] 1.2 Implement FNetcContext (RAII, TCP and UDP modes, Compress/Decompress TArrayView API)
- [ ] 1.3 Implement FNetcTrainer (train from TArray<TArray<uint8>>, save/load dict)
- [ ] 1.4 Implement FNetcResult (error propagation compatible with UE_LOG and ensureMsgf)
- [ ] 1.5 Write unit tests (standalone C++17, no UE5 dependency) for all wrappers

## 2. UE5 Plugin
- [ ] 2.1 Write NetcPlugin.uplugin descriptor (ThirdParty module declaration)
- [ ] 2.2 Write NetcPlugin.Build.cs (UBT build rules, platform-conditional libnetc linking)
- [ ] 2.3 Pre-build libnetc.a for Win64, Linux, Mac (CMake cross-compile in CI)
- [ ] 2.4 Write platform detection for SIMD flags (MSVC /arch:AVX2, GCC -mavx2)

## 3. UE5 Automation Tests
- [ ] 3.1 Write IMPLEMENT_SIMPLE_AUTOMATION_TEST for FNetcDict load/save round-trip
- [ ] 3.2 Write test for FNetcContext compress/decompress on game state packet (WL-001 equivalent)
- [ ] 3.3 Write test for FNetcContext TCP stateful mode (multi-packet sequence)
- [ ] 3.4 Write test for FNetcContext UDP stateless mode
- [ ] 3.5 Write test for thread safety (multiple FNetcContext sharing one FNetcDict)

## 4. Documentation
- [ ] 4.1 Write docs/specs/sdk-cpp.md (installation, quick start, API reference, UE5 examples)
- [ ] 4.2 Write sdk/cpp/README.md (plugin installation instructions)
- [ ] 4.3 Update root README.md SDK section with accurate UE5 code examples
