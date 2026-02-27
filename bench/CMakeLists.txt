# bench/CMakeLists.txt — Benchmark harness build (Phase 5)
#
# Optional dependencies detected via find_package / pkg-config:
#   - zlib   (NETC_BENCH_WITH_ZLIB, default auto-detect)
#   - lz4    (NETC_BENCH_WITH_LZ4,  default auto-detect)
#   - zstd   (NETC_BENCH_WITH_ZSTD, default auto-detect)
#   - OodleNetwork (NETC_BENCH_WITH_OODLE, parent CMakeLists.txt, default OFF)
#
# The bench binary always links against libm (for sqrt in bench_stats.c).

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Core bench sources (no optional deps required)
# ============================================================================
set(BENCH_SOURCES
    bench_corpus.c
    bench_stats.c
    bench_reporter.c
    bench_netc.c
    bench_runner.c
    bench_main.c
)

add_executable(bench ${BENCH_SOURCES})

target_include_directories(bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}          # bench/*.h
        ${CMAKE_SOURCE_DIR}/include          # netc.h
        ${CMAKE_SOURCE_DIR}/src              # internal headers if needed
)

target_link_libraries(bench PRIVATE netc)

# libm for sqrt / exp in bench_corpus.c and bench_stats.c
if(UNIX)
    target_link_libraries(bench PRIVATE m)
endif()

# Inherit parent SIMD flags
if(NETC_SIMD_FLAGS)
    target_compile_options(bench PRIVATE ${NETC_SIMD_FLAGS})
endif()

# MSVC: suppress C4996 for gmtime (use gmtime_s on Windows)
if(MSVC)
    target_compile_definitions(bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================================
# Optional: zlib adapter
# ============================================================================
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    message(STATUS "bench: zlib found — building bench_zlib adapter")
    target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_ZLIB=1)
    target_link_libraries(bench PRIVATE ZLIB::ZLIB)
endif()

# ============================================================================
# Optional: LZ4 adapter
# ============================================================================
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET liblz4)
    if(LZ4_FOUND)
        message(STATUS "bench: lz4 found — building bench_lz4 adapter")
        target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_LZ4=1)
        target_include_directories(bench PRIVATE ${LZ4_INCLUDE_DIRS})
        target_link_libraries(bench PRIVATE ${LZ4_LIBRARIES})
    endif()
endif()

# ============================================================================
# Optional: Zstd adapter
# ============================================================================
find_package(zstd QUIET)
if(zstd_FOUND)
    message(STATUS "bench: zstd found — building bench_zstd adapter")
    target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_ZSTD=1)
    target_link_libraries(bench PRIVATE zstd::libzstd_static)
elseif(PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
    if(ZSTD_FOUND)
        message(STATUS "bench: zstd (pkg-config) found")
        target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_ZSTD=1)
        target_include_directories(bench PRIVATE ${ZSTD_INCLUDE_DIRS})
        target_link_libraries(bench PRIVATE ${ZSTD_LIBRARIES})
    endif()
endif()

# ============================================================================
# Optional: OodleNetwork adapter (configured by parent CMakeLists.txt)
# ============================================================================
if(NETC_BENCH_WITH_OODLE AND DEFINED UE5_OODLE_SDK)
    set(OODLE_INC "${UE5_OODLE_SDK}/include")
    set(OODLE_LIB_DIR "${UE5_OODLE_SDK}/lib/Win64")
    if(EXISTS "${OODLE_INC}" AND EXISTS "${OODLE_LIB_DIR}")
        message(STATUS "bench: OodleNetwork SDK found at ${UE5_OODLE_SDK}")
        target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_OODLE=1)
        target_include_directories(bench PRIVATE "${OODLE_INC}")
        target_link_directories(bench PRIVATE "${OODLE_LIB_DIR}")
        target_link_libraries(bench PRIVATE oo2net_win64)
    else()
        message(WARNING "bench: OodleNetwork SDK path invalid: ${UE5_OODLE_SDK}")
    endif()
endif()

# ============================================================================
# Install bench binary
# ============================================================================
install(TARGETS bench RUNTIME DESTINATION bin)
