# Proposal: Implement Benchmark Harness (Comparative Performance Suite)

## Why

netc's core claim is that it outperforms general-purpose compressors (zlib, LZ4, Zstd, Huffman) on structured binary packet workloads. Without a rigorous, reproducible benchmark harness, this claim cannot be validated, and the RFC-002 CI gates cannot be enforced. The benchmark harness is also the primary tool for identifying regressions during development and for communicating performance results to users evaluating netc.

## What Changes

- Implement 8 workload corpus generators (WL-001 through WL-008) per RFC-002 §3
- Implement RDTSC/clock_gettime high-resolution timing infrastructure
- Implement adapters for zlib, LZ4, Zstd, Huffman (reference), Snappy
- Implement statistical aggregation (p50, p90, p99, p999, mean, stddev)
- Implement CSV, JSON, and table output reporters
- Implement CI gate checker (--ci-check, exit code 0/1) per RFC-002 §6
- Implement regression tracking against stored baselines
- CMake integration: `make bench` target with optional dependency detection

## Impact

- Affected specs: bench/spec.md (new)
- Affected code: bench/ directory (new), CMakeLists.txt (updated — bench target)
- Breaking change: NO (bench is a separate binary, not part of libnetc)
- User benefit: Reproducible, publishable proof that netc outperforms alternatives; CI gates prevent performance regressions from being merged
