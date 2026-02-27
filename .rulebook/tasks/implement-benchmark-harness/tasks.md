## 1. Timing Infrastructure
- [x] 1.1 Implement RDTSC timer with TSC frequency calibration (x86)
- [x] 1.2 Implement clock_gettime(CLOCK_MONOTONIC_RAW) fallback (Linux/macOS)
- [x] 1.3 Implement QueryPerformanceCounter timer (Windows)
- [x] 1.4 Implement percentile calculator (p50, p90, p99, p999, mean, stddev)

## 2. Corpus Generators (WL-001 to WL-008)
- [x] 2.1 WL-001: Game state packet 64B (player position/velocity/flags)
- [x] 2.2 WL-002: Game state packet 128B (extended entity state)
- [x] 2.3 WL-003: Game state packet 256B (full snapshot)
- [x] 2.4 WL-004: Financial tick data 32B (price, volume, symbol, timestamp)
- [x] 2.5 WL-005: Telemetry packet 512B (sensor aggregation)
- [x] 2.6 WL-006: Random data 128B (crypto-random, high entropy)
- [x] 2.7 WL-007: Highly repetitive 128B (all-zeros, counter, pattern)
- [x] 2.8 WL-008: Mixed traffic (weighted random selection from WL-001..006)

## 3. Compressor Adapters
- [x] 3.1 netc adapter (compress + decompress, all configs)
- [x] 3.2 zlib adapter (level=1 and level=6)
- [x] 3.3 LZ4 adapter (fast and HC modes)
- [x] 3.4 Zstd adapter (level=1, level=3, level=1+dict)
- [ ] 3.5 Reference Huffman adapter (static, trained on same corpus)
- [ ] 3.6 Snappy adapter (optional, skip if not found)
- [ ] 3.7 OodleNetwork adapter (bench_oodle.c, compiled only if NETC_BENCH_WITH_OODLE=ON)
  - UDP stateless: OodleNetwork1UDP_Train + OodleNetwork1UDP_Encode/Decode
  - TCP stateful: OodleNetwork1TCP_Train + OodleNetwork1TCP_Encode/Decode
  - SDK path via CMake variable UE5_OODLE_SDK or env var
  - Links against oo2net_win64.lib (Windows) / liboo2netlinux64.a (Linux)
  - Train on same corpus as netc (same packet count, same seed)

## 4. Benchmark Runner
- [x] 4.1 Implement single-packet latency benchmark loop (100,000 iterations)
- [ ] 4.2 Implement throughput benchmark (sustained MB/s over full corpus)
- [ ] 4.3 Implement Mpps benchmark (1,000,000 packets per second measurement)
- [ ] 4.4 Implement multi-core scaling benchmark (1, 2, 4, 8, 16 threads)
- [x] 4.5 Implement warmup phase (1,000 iterations before timing)

## 5. Output Reporters
- [x] 5.1 Table reporter (human-readable, aligned columns)
- [x] 5.2 CSV reporter (per RFC-002 §7.3 schema)
- [x] 5.3 JSON reporter (per RFC-002 §7.4 schema, includes system info)

## 6. CI Gate Checker
- [x] 6.1 Implement --ci-check mode (runs all PERF-*, RATIO-*, SAFETY-*, MEM-* gates)
- [x] 6.2 Implement COMP-* comparison gates (netc vs LZ4, zlib, Zstd)
- [ ] 6.3 Implement OODLE-* comparison gates (netc vs OodleNetwork, when --with-oodle is active)
- [ ] 6.4 Implement baseline regression check (±5% warning, ±15% fail)
- [ ] 6.5 Implement baseline save/load (bench/baselines/*.json)
- [x] 6.6 Exit code 0 on pass, 1 on fail

## 7. CLI and Build Integration
- [x] 7.1 Implement CLI argument parsing (--workload, --compressor, --count, --seed, --format, --output, --ci-check, --with-oodle, --oodle-sdk, --oodle-htbits, --oodle-gates)
- [x] 7.2 CMake target: bench (optional deps: zlib, lz4, zstd detected via pkg-config)
- [x] 7.3 CMake option: NETC_BENCH_WITH_OODLE (default OFF); when ON, requires UE5_OODLE_SDK path
- [x] 7.4 GitHub Actions: benchmark CI job (runs --ci-check, uploads results artifact; no Oodle in CI)
- [x] 7.5 Write bench README with usage examples including Oodle comparison instructions
