# Spec: Benchmark Harness

## ADDED Requirements

### Requirement: Reproducible Corpus Generation
The benchmark corpus generator SHALL produce deterministic output given the same workload ID and seed.

#### Scenario: Same seed produces same corpus
Given workload=WL-001, seed=42, count=100000
When bench_corpus_generate is called twice
Then both calls SHALL produce byte-for-byte identical packet sequences

#### Scenario: Different seeds produce different corpora
Given workload=WL-001, count=100000
When called with seed=42 and seed=43
Then the packet sequences SHALL differ

### Requirement: CI Gate Enforcement
The --ci-check mode SHALL enforce all RFC-002 §6 pass/fail criteria and return a machine-readable exit code.

#### Scenario: All gates pass
Given netc meeting all RFC-002 §6.1 performance targets
When ./bench --ci-check is executed
Then the process SHALL exit with code 0
And a summary of all gates (PASS/FAIL) SHALL be printed to stdout

#### Scenario: One gate fails — blocks CI
Given netc where compress throughput is 1.5 GB/s (below 2 GB/s target — PERF-01)
When ./bench --ci-check is executed
Then the process SHALL exit with code 1
And the failing gate (PERF-01) SHALL be clearly identified in the output

### Requirement: Fair Comparison
All compressor adapters SHALL be tested under identical conditions.

#### Scenario: Cache pre-warming applied equally
Given compressors netc and LZ4
When both are benchmarked on WL-001
Then both SHALL have 1,000 warmup iterations before timing
And the same corpus SHALL be used for both

#### Scenario: Dictionary-capable compressors use trained dict
Given Zstd and netc both supporting dictionaries
When both are benchmarked on WL-001
Then both SHALL use a dictionary trained on the same 50,000-packet corpus

### Requirement: Statistical Reporting
The harness SHALL compute and report p50, p90, p99, p999, mean, and stddev for all timing measurements.

#### Scenario: Percentile computation
Given 100,000 timing samples
When statistics are computed
Then p99 SHALL represent the 99,000th smallest value (sorted)
And stddev SHALL be the population standard deviation

### Requirement: Regression Detection
The harness SHALL detect performance regressions against stored baselines.

#### Scenario: 5% regression triggers warning
Given a stored baseline where netc compress throughput = 2500 MB/s
When the current run measures 2350 MB/s (6% below baseline)
Then a WARNING SHALL be printed to stderr
And exit code SHALL be 0 (warning only, not failure)

#### Scenario: 15% regression fails CI
Given a stored baseline where netc compress throughput = 2500 MB/s
When the current run measures 2050 MB/s (18% below baseline)
Then the REGRESSION-01 gate SHALL FAIL
And exit code SHALL be 1
