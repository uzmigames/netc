# Profile-Guided Optimization (PGO) Build Workflow

netc's CMakeLists.txt includes a three-step PGO build workflow for GCC and clang.
PGO uses real-world workload profiles to guide the compiler's branch prediction,
inlining, and code layout decisions — typically yielding **5–15% throughput improvement**
on the hot compression/decompression paths.

MSVC is not currently supported for PGO via CMake (`NETC_PGO_INSTRUMENT` is a no-op on MSVC).

---

## Overview: Three-Step Workflow

```
Step 1: Instrument build   →  binary records branch counts during execution
Step 2: Profile run        →  run bench with representative workload
Step 3: Optimized build    →  compiler uses profile data to optimize layout
```

---

## Step 1 — Instrument Build

Configure with `NETC_PGO_INSTRUMENT=ON`. This adds `-fprofile-generate` to the
netc library and all linked binaries.

```bash
cmake -B build-pgo-instrument \
  -DCMAKE_BUILD_TYPE=Release \
  -DNETC_BUILD_BENCH=ON \
  -DNETC_PGO_INSTRUMENT=ON \
  -DNETC_PGO_DATA_DIR=$(pwd)/pgo_data

cmake --build build-pgo-instrument -j$(nproc)
```

The instrumented binary records counters into `pgo_data/*.gcda` (GCC) or
`pgo_data/*.profraw` (clang) files during execution.

---

## Step 2 — Profile Run

Run the benchmark harness with the most representative workloads. Use a large
`--count` so branch frequencies stabilize.

```bash
# Run all workloads with dictionary training (the hot path we want to optimize)
./build-pgo-instrument/bench/bench \
  --compressor=netc \
  --count=500000 \
  --warmup=10000 \
  --seed=42 \
  --train=50000

# If using clang, merge the raw profiles first:
llvm-profdata merge -output=pgo_data/netc.profdata pgo_data/*.profraw
```

For GCC, `.gcda` files are written directly — no merge step is needed.

---

## Step 3 — Optimized Build

Configure with `NETC_PGO_OPTIMIZE=ON` pointing to the profile data directory.

```bash
cmake -B build-pgo-optimized \
  -DCMAKE_BUILD_TYPE=Release \
  -DNETC_BUILD_BENCH=ON \
  -DNETC_PGO_OPTIMIZE=ON \
  -DNETC_PGO_DATA_DIR=$(pwd)/pgo_data

cmake --build build-pgo-optimized -j$(nproc)
```

The compiler reads the `.gcda` / `.profdata` files and applies profile-guided
inlining, branch layout, and register allocation to the netc library.

---

## Verifying the Speedup

Compare the PGO and non-PGO builds on WL-001 (the primary target workload):

```bash
# Non-PGO baseline
cmake -B build-baseline -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_BENCH=ON
cmake --build build-baseline -j$(nproc)
./build-baseline/bench/bench --workload=WL-001 --count=200000 --format=table

# PGO optimized
./build-pgo-optimized/bench/bench --workload=WL-001 --count=200000 --format=table
```

Target: PGO build compress throughput ≥ baseline × 1.05 (≥ 5% improvement).

---

## Clang vs GCC Notes

| | GCC | Clang |
|--|-----|-------|
| Instrument flag | `-fprofile-generate=<dir>` | `-fprofile-generate=<dir>` |
| Profile files | `*.gcda` in `<dir>` | `*.profraw` in `<dir>` |
| Merge step | Not needed | `llvm-profdata merge` |
| Optimize flag | `-fprofile-use=<dir>` | `-fprofile-use=<dir>.profdata` |
| Extra flag | — | `-fprofile-correction` (handles stale counts) |

The CMake option `NETC_PGO_OPTIMIZE` passes `-fprofile-correction` automatically
for both compilers to handle any stale or mismatched profile data.

---

## CI Integration

PGO is not run automatically in the main CI pipeline (the three-step process
requires sequential builds). To run it manually in CI, trigger the workflow with
`workflow_dispatch` and pass `PGO=true` as an input, or add a scheduled weekly job:

```yaml
# Example: weekly PGO benchmark job (add to ci.yml)
on:
  schedule:
    - cron: '0 4 * * 1'   # every Monday at 04:00 UTC

jobs:
  pgo-benchmark:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Build instrumented
        run: |
          cmake -B build-inst -DCMAKE_BUILD_TYPE=Release \
            -DNETC_BUILD_BENCH=ON -DNETC_PGO_INSTRUMENT=ON \
            -DNETC_PGO_DATA_DIR=$PWD/pgo_data
          cmake --build build-inst -j$(nproc)
      - name: Profile run
        run: |
          ./build-inst/bench/bench --count=500000 --warmup=10000
      - name: Build optimized
        run: |
          cmake -B build-pgo -DCMAKE_BUILD_TYPE=Release \
            -DNETC_BUILD_BENCH=ON -DNETC_PGO_OPTIMIZE=ON \
            -DNETC_PGO_DATA_DIR=$PWD/pgo_data
          cmake --build build-pgo -j$(nproc)
      - name: Compare PGO vs baseline
        run: |
          cmake -B build-base -DCMAKE_BUILD_TYPE=Release -DNETC_BUILD_BENCH=ON
          cmake --build build-base -j$(nproc)
          echo "=== Baseline ===" && ./build-base/bench/bench --workload=WL-001 --count=200000
          echo "=== PGO ===" && ./build-pgo/bench/bench --workload=WL-001 --count=200000
```

---

## Windows (MSVC)

MSVC PGO uses a different toolchain (`/GL`, `/LTCG`, `pgoinstrument.exe`).
The `NETC_PGO_INSTRUMENT` / `NETC_PGO_OPTIMIZE` CMake options are **no-ops on MSVC**.

For MSVC PGO, use the Visual Studio PGO wizard or the PGORT toolchain manually:

```
1. Build with /GL (whole-program optimization) — add to CMakeLists manually
2. Run bench.exe to generate .pgd files in the output directory
3. Rebuild with /LTCG:PGO pointing to the .pgd files
```

See [MSVC PGO documentation](https://learn.microsoft.com/en-us/cpp/build/profile-guided-optimizations).
