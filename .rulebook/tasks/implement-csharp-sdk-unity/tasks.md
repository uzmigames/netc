## 1. P/Invoke Layer
- [x] 1.1 Write NetcNative.cs (DllImport for all public netc functions, CallingConvention.Cdecl)
- [ ] 1.2 Implement platform-conditional DLL loading (Win64 .dll, Linux .so, macOS .dylib, Android .so, iOS static)
- [x] 1.3 Write unit tests for P/Invoke declarations (xUnit, dotnet test)

## 2. Core C# Wrappers
- [x] 2.1 Implement NetcDict (IDisposable, LoadFromBytes(ReadOnlySpan<byte>), Save)
- [x] 2.2 Implement NetcContext (IDisposable, Stateful/Stateless mode, Compress/Decompress Span<byte> API)
- [x] 2.3 Implement NetcTrainer (AddPacket, Train, Reset)
- [x] 2.4 Implement NetcException (maps netc_result_t error codes to typed exceptions)
- [x] 2.5 Write unit tests: 56 xUnit tests (roundtrip, stateless, error paths, disposal, trainer)

## 3. Native Binary Packaging
- [ ] 3.1 CMake cross-compile targets: Win64, Linux x86_64, macOS ARM64
- [ ] 3.2 Android NDK build: arm64-v8a (with NEON), x86_64
- [ ] 3.3 iOS static library: arm64
- [ ] 3.4 CI: build all native binaries and publish to GitHub Releases
- [ ] 3.5 Write NuGet .csproj package descriptor with native binary bundling per RID

## 4. Documentation
- [ ] 4.1 Write sdk/csharp/README.md (NuGet install, API reference, examples)
- [ ] 4.2 Update root README.md SDK section with C# examples
