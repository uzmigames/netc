# Makefile — Simple wrapper around CMake for common tasks.
#
# Usage:
#   make            — Configure (Release) + build
#   make debug      — Configure (Debug + ASan/UBSan) + build
#   make test       — Run all tests
#   make bench      — Build and run benchmark harness
#   make fetch-deps — Download Unity test framework
#   make clean      — Remove build directory
#   make rebuild    — clean + build

BUILD_DIR ?= build
BUILD_DIR_DEBUG ?= build-debug

CMAKE ?= cmake
CTEST ?= ctest

.PHONY: all release debug test bench fetch-deps clean rebuild

all: release

# -----------------------------------------------------------------------------
# Release build
# -----------------------------------------------------------------------------
release: $(BUILD_DIR)/Makefile
	$(CMAKE) --build $(BUILD_DIR) --config Release -j

$(BUILD_DIR)/Makefile:
	$(CMAKE) -B $(BUILD_DIR) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DNETC_BUILD_TESTS=ON \
	    -DNETC_BUILD_BENCH=ON

# -----------------------------------------------------------------------------
# Debug build (ASan + UBSan)
# -----------------------------------------------------------------------------
debug: $(BUILD_DIR_DEBUG)/Makefile
	$(CMAKE) --build $(BUILD_DIR_DEBUG) --config Debug -j

$(BUILD_DIR_DEBUG)/Makefile:
	$(CMAKE) -B $(BUILD_DIR_DEBUG) \
	    -DCMAKE_BUILD_TYPE=Debug \
	    -DNETC_BUILD_TESTS=ON \
	    -DNETC_ENABLE_SANITIZERS=ON

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
test: release
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure -j

test-debug: debug
	$(CTEST) --test-dir $(BUILD_DIR_DEBUG) --output-on-failure -j

# -----------------------------------------------------------------------------
# Benchmark harness
# -----------------------------------------------------------------------------
bench: release
	@echo "Benchmark harness will be available in Phase 5"
	@echo "Run: ./$(BUILD_DIR)/bench/bench --workload WL-001 --format table"

# -----------------------------------------------------------------------------
# Fetch dependencies (Unity test framework)
# -----------------------------------------------------------------------------
fetch-deps: $(BUILD_DIR)/Makefile
	$(CMAKE) --build $(BUILD_DIR) --target fetch-unity

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	$(CMAKE) -E remove_directory $(BUILD_DIR)
	$(CMAKE) -E remove_directory $(BUILD_DIR_DEBUG)

rebuild: clean all
