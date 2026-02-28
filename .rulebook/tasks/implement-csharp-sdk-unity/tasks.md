## 1. P/Invoke Layer
- [x] 1.1 Write NetcNative.cs (DllImport for all public netc functions, CallingConvention.Cdecl)
- [ ] 1.2 Implement platform-conditional DLL loading (Win64 .dll, Linux .so, macOS .dylib, Android .so, iOS static)
- [x] 1.3 Write unit tests for P/Invoke declarations (xUnit, dotnet test)

## 2. Core C# Wrappers
- [x] 2.1 Implement NetcDict (IDisposable, LoadFromBytes(ReadOnlySpan<byte>), LoadFromFile, Train)
- [x] 2.2 Implement NetcContext (IDisposable, TCP/UDP mode, Compress/Decompress Span<byte> API)
- [x] 2.3 Implement NetcTrainer (Train from IEnumerable<ReadOnlyMemory<byte>>, SaveToBytes)
- [x] 2.4 Implement NetcException (maps netc_result_t error codes to typed exceptions)
- [x] 2.5 Write unit tests: round-trip all workloads (WL-001..WL-008 equivalent), thread safety

## 3. Unity Adapter
- [ ] 3.1 Implement NetcMirrorTransport (extends Mirror Transport, wraps NetcContext per connection)
- [ ] 3.2 Implement NetcFishNetTransport (extends FishNet Transport)
- [ ] 3.3 Implement NetcNGOTransport (extends Unity Netcode for GameObjects INetworkStreamDriverConstructor)
- [ ] 3.4 Write Unity PlayMode tests for each transport adapter (compress/decompress round-trip)
- [ ] 3.5 Write Unity package.json and asmdef files for UPM distribution

## 4. Native Binary Packaging
- [ ] 4.1 CMake cross-compile targets: Win64, Linux x86_64, macOS ARM64
- [ ] 4.2 Android NDK build: arm64-v8a (with NEON), x86_64
- [ ] 4.3 iOS static library: arm64
- [ ] 4.4 CI: build all native binaries and publish to GitHub Releases as artifacts
- [ ] 4.5 Write NuGet .csproj package descriptor with native binary bundling per RID

## 5. Documentation
- [ ] 5.1 Write docs/sdk-csharp.md (P/Invoke internals, Unity quick start)
- [ ] 5.2 Write sdk/csharp/README.md (NuGet install, UPM install)
- [ ] 5.3 Update root README.md SDK section with final verified code examples
