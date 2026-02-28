# netc C++ SDK

Standalone C++17 RAII wrappers for the netc compression library. No engine dependencies.

## Build

The C++ SDK is built as part of the main netc CMake project:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_CPP_SDK=ON
cmake --build build --config Release
```

This produces the `netc_cpp` static library and links against `netc` (static).

### Run Tests

```bash
ctest --test-dir build -C Release --output-on-failure -R test_cpp_sdk
```

47 tests covering Dict, Context, Compress/Decompress, Trainer, RAII safety, and error paths.

## API Reference

All types are in `namespace netc`. Include `<netc.hpp>` for the umbrella header.

### `netc::Dict`

Move-only dictionary wrapper. Thread-safe for concurrent reads.

```cpp
#include <netc.hpp>

// Load from memory
netc::Dict dict;
netc::Result r = netc::Dict::LoadFromBytes(blob_data, blob_size, dict);

// Load from file
netc::Dict dict2;
netc::Dict::LoadFromFile("trained.dict", dict2);

// Save
std::vector<uint8_t> bytes;
dict.SaveToBytes(bytes);
dict.SaveToFile("output.dict");

// Inspect
uint8_t model_id = dict.GetModelId();  // 1-254
bool valid = dict.IsValid();
```

### `netc::Context`

Move-only compression context. NOT thread-safe — use one per connection per thread.

```cpp
#include <netc.hpp>

// TCP stateful mode with delta + compact headers
netc::Context ctx(dict, netc::Mode::TCP, /*level=*/5,
    NETC_CFG_FLAG_DELTA | NETC_CFG_FLAG_COMPACT_HDR);

// Compress
std::vector<uint8_t> compressed;
compressed.reserve(netc::Context::MaxCompressedSize(src_size));
netc::Result r = ctx.Compress(src, src_size, compressed);

// Decompress
std::vector<uint8_t> recovered;
r = ctx.Decompress(compressed.data(), compressed.size(), recovered);

// UDP stateless (no context state)
std::vector<uint8_t> out;
netc::Context::CompressStateless(dict, src, src_size, out);
netc::Context::DecompressStateless(dict, out.data(), out.size(), recovered);

// Utilities
ctx.Reset();                          // Reset ring buffer, keep dict
netc::SimdLevel lvl = ctx.GetSimdLevel();  // Generic/SSE42/AVX2/NEON
netc::Stats stats;
ctx.GetStats(stats);
double ratio = stats.AverageRatio();
```

### `netc::Trainer`

Corpus management and dictionary training.

```cpp
#include <netc.hpp>

netc::Trainer trainer;

// Add packets one at a time
trainer.AddPacket(pkt_data, pkt_size);

// Or add a batch
std::vector<std::vector<uint8_t>> corpus = { ... };
trainer.AddPackets(corpus);

size_t count = trainer.GetCorpusCount();

// Train
netc::Dict dict;
netc::Result r = trainer.Train(/*model_id=*/1, dict);

trainer.Reset();  // Clear corpus
```

### `netc::Result`

Enum mapping all `netc_result_t` error codes:

| Value | Name | Description |
|-------|------|-------------|
| 0 | `Ok` | Success |
| -1 | `ErrInvalidArg` | NULL pointer or invalid parameter |
| -2 | `ErrBufferTooSmall` | Output buffer insufficient |
| -3 | `ErrCorrupt` | Corrupted or truncated data |
| -4 | `ErrModelMismatch` | Dict model_id != packet model_id |
| -5 | `ErrNoMem` | Memory allocation failed |
| -6 | `ErrDictRequired` | Operation requires a dictionary |
| -7 | `ErrNotReady` | Context not initialized |

```cpp
const char* msg = netc::ResultToString(r);  // "Ok", "ErrCorrupt", etc.
```

## File Structure

```
sdk/cpp/
├── include/
│   ├── netc.hpp              # Umbrella header
│   └── netc/
│       ├── Result.hpp        # Result enum + ResultToString
│       ├── Dict.hpp          # Dict class
│       ├── Context.hpp       # Context class + Mode/SimdLevel/Stats
│       └── Trainer.hpp       # Trainer class
├── src/
│   ├── Dict.cpp
│   ├── Context.cpp
│   └── Trainer.cpp
├── tests/
│   └── test_cpp_sdk.cpp      # 47 unit tests
└── README.md
```

## License

Apache License 2.0 — see [LICENSE](../../LICENSE).
