# C Core Architect Memory

## Project: netc - Network Packet Compression Library

### Key Architecture
- tANS entropy coding with 12-bit (4096) and 10-bit (1024) table sizes
- LZP prediction + delta encoding pipeline
- Compact packet headers (2-4B) vs legacy (8B)
- MSVC / Windows 10 build with CMake, tests use Unity framework

### Important File Locations
- `src/algo/netc_tans.h` / `.c`: tANS table build/encode/decode (12-bit and 10-bit)
- `src/algo/netc_adaptive.h` / `.c`: Adaptive cross-packet frequency learning
- `src/core/netc_compress.c`: Main compressor (~1600+ lines)
- `src/core/netc_decompress.c`: Main decompressor (~930+ lines)
- `src/core/netc_internal.h`: Compact packet type table (256 entries), ctx struct, adaptive constants
- `src/core/netc_ctx.c`: Context lifecycle (create/destroy/reset) incl. adaptive alloc
- `include/netc.h`: Public API, algorithm constants, cfg flags
- `CMakeLists.txt`: Build system, test registration via `add_netc_test()`

### MSVC Gotchas
- **C4310 warning**: `TEST_ASSERT_EQUAL_HEX8(0xB0, val)` triggers "constant truncation" warning treated as error. Fix: cast to `(uint8_t)0xB0`.
- `/WX` (warnings as errors) is always enabled for MSVC builds.

### Compact Packet Type Table Layout
- `0x00-0x0F`: RAW + bucket
- `0x10-0x1F`: TANS + bucket
- `0x30-0x3F`: TANS + DELTA + bucket
- `0x50-0x5F`: TANS + LZP + bucket
- `0x70-0x8F`: LZP compact types
- `0x90-0xAF`: LZP + BIGRAM compact types
- `0xB0-0xBF`: TANS_10 + bucket
- `0xC0-0xCF`: TANS_10 + DELTA + bucket

### Algorithm Constants
- `NETC_ALG_TANS = 0x01`
- `NETC_ALG_RANS = 0x02`
- `NETC_ALG_LZP  = 0x05`
- `NETC_ALG_TANS_10 = 0x06`

### tANS Spread Steps
- 12-bit: step = 2731 (coprime with 4096)
- 10-bit: step = 643 (coprime with 1024)

### Dict Versioning (v5 upgrade, 2026-02-28)
- **v4**: 4 static bigram classes (prev_byte >> 6), `NETC_BIGRAM_CTX_COUNT_V4 = 4`
- **v5**: 8 trained bigram classes via `bigram_class_map[256]`, `NETC_BIGRAM_CTX_COUNT = 8`
- `NETC_DICT_VERSION = 5`, `NETC_DICT_VERSION_V4 = 4` for backward compat
- `netc_bigram_class(prev_byte, class_map)` — 2-arg signature, NULL class_map → v4 fallback
- Dict struct: `bigram_class_map[256]`, `bigram_class_count` (4 or 8) fields added
- v5 blob: header(8) + class_map(256) + unigram(8192) + bigram(65536) + [LZP] + CRC(4)
- v4 blob: header(8) + unigram(8192) + bigram(32768) + [LZP] + CRC(4)
- v4 load builds default class_map from prev>>6, sets bigram_class_count=4
- Training: 2-pass — pass 1 builds class_map (sort prev_bytes by peak next-sym, 8 groups of 32), pass 2 accumulates freqs
