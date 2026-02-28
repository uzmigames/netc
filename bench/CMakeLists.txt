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
    bench_compressor.c
    bench_netc.c
    bench_runner.c
    bench_zlib.c
    bench_lz4.c
    bench_zstd.c
    bench_huffman.c
    bench_snappy.c
    bench_oodle.c
    bench_throughput.c
    bench_multicore.c
    bench_baseline.c
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

# Inherit PGO flags for correct linking
if(NETC_PGO_INSTRUMENT AND NOT MSVC)
    target_compile_options(bench PRIVATE -fprofile-generate=${NETC_PGO_DATA_DIR})
    target_link_options(bench   PRIVATE -fprofile-generate=${NETC_PGO_DATA_DIR})
elseif(NETC_PGO_OPTIMIZE AND NOT MSVC)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set(_PROFDATA "${NETC_PGO_DATA_DIR}/default.profdata")
        target_compile_options(bench PRIVATE -fprofile-instr-use=${_PROFDATA} -Wno-error=profile-instr-unprofiled)
        target_link_options(bench   PRIVATE -fprofile-instr-use=${_PROFDATA})
    else()
        target_compile_options(bench PRIVATE -fprofile-use=${NETC_PGO_DATA_DIR} -fprofile-correction -Wno-error=missing-profile)
        target_link_options(bench   PRIVATE -fprofile-use=${NETC_PGO_DATA_DIR})
    endif()
endif()

# Inherit parent SIMD flags
if(NETC_SIMD_FLAGS)
    target_compile_options(bench PRIVATE ${NETC_SIMD_FLAGS})
endif()

# MSVC: suppress C4996 for gmtime (use gmtime_s on Windows)
if(MSVC)
    target_compile_definitions(bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# POSIX: clock_gettime requires _POSIX_C_SOURCE >= 199309L
if(UNIX)
    target_compile_definitions(bench PRIVATE _POSIX_C_SOURCE=199309L)
endif()

# GCC/Clang: relax -Werror for bench code (not core library)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(bench PRIVATE -Wno-error)
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
# Optional: LZ4 adapter — try vcpkg/CMake config first, then pkg-config
# ============================================================================
find_package(lz4 CONFIG QUIET)
if(lz4_FOUND)
    message(STATUS "bench: lz4 found (CMake config) — building bench_lz4 adapter")
    target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_LZ4=1)
    target_link_libraries(bench PRIVATE lz4::lz4)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 QUIET liblz4)
        if(LZ4_FOUND)
            message(STATUS "bench: lz4 found (pkg-config) — building bench_lz4 adapter")
            target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_LZ4=1)
            target_include_directories(bench PRIVATE ${LZ4_INCLUDE_DIRS})
            target_link_libraries(bench PRIVATE ${LZ4_LIBRARIES})
        endif()
    endif()
endif()

# ============================================================================
# Optional: Zstd adapter — try vcpkg/CMake config first, then pkg-config
# ============================================================================
find_package(zstd CONFIG QUIET)
if(zstd_FOUND OR ZSTD_FOUND)
    message(STATUS "bench: zstd found (CMake config) — building bench_zstd adapter")
    target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_ZSTD=1)
    if(TARGET zstd::libzstd_static)
        target_link_libraries(bench PRIVATE zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        target_link_libraries(bench PRIVATE zstd::libzstd_shared)
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD QUIET libzstd)
        if(ZSTD_FOUND)
            message(STATUS "bench: zstd found (pkg-config) — building bench_zstd adapter")
            target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_ZSTD=1)
            target_include_directories(bench PRIVATE ${ZSTD_INCLUDE_DIRS})
            target_link_libraries(bench PRIVATE ${ZSTD_LIBRARIES})
        endif()
    endif()
endif()

# ============================================================================
# Optional: Snappy adapter
# ============================================================================
if(PkgConfig_FOUND)
    pkg_check_modules(SNAPPY QUIET snappy)
    if(SNAPPY_FOUND)
        message(STATUS "bench: snappy found — building bench_snappy adapter")
        target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_SNAPPY=1)
        target_include_directories(bench PRIVATE ${SNAPPY_INCLUDE_DIRS})
        target_link_libraries(bench PRIVATE ${SNAPPY_LIBRARIES})
    endif()
endif()
if(NOT SNAPPY_FOUND)
    find_library(SNAPPY_LIB snappy)
    if(SNAPPY_LIB)
        message(STATUS "bench: snappy found via find_library")
        target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_SNAPPY=1)
        target_link_libraries(bench PRIVATE ${SNAPPY_LIB})
    endif()
endif()

# ============================================================================
# Optional: OodleNetwork adapter (configured by parent CMakeLists.txt)
# UE5_OODLE_SDK or NETC_OODLE_SDK_PATH env variable must point to Oodle SDK root.
# ============================================================================
if(NETC_BENCH_WITH_OODLE)
    # Prefer CMake variable, fall back to env variable
    if(NOT DEFINED UE5_OODLE_SDK AND DEFINED ENV{NETC_OODLE_SDK_PATH})
        set(UE5_OODLE_SDK "$ENV{NETC_OODLE_SDK_PATH}")
    endif()

    if(DEFINED UE5_OODLE_SDK)
        set(OODLE_INC "${UE5_OODLE_SDK}/include")
        if(WIN32)
            set(OODLE_LIB_DIR "${UE5_OODLE_SDK}/lib/Win64")
            set(OODLE_LIB_NAME "oo2net_win64")
        else()
            set(OODLE_LIB_DIR "${UE5_OODLE_SDK}/lib/Linux")
            set(OODLE_LIB_NAME "oo2netlinux64")
        endif()

        if(EXISTS "${OODLE_INC}" AND EXISTS "${OODLE_LIB_DIR}")
            message(STATUS "bench: OodleNetwork SDK found at ${UE5_OODLE_SDK}")
            target_compile_definitions(bench PRIVATE NETC_BENCH_WITH_OODLE=1)
            target_include_directories(bench PRIVATE "${OODLE_INC}")
            target_link_directories(bench PRIVATE "${OODLE_LIB_DIR}")
            target_link_libraries(bench PRIVATE ${OODLE_LIB_NAME})
        else()
            message(WARNING "bench: OodleNetwork SDK path invalid: ${UE5_OODLE_SDK}")
        endif()
    else()
        message(WARNING "bench: NETC_BENCH_WITH_OODLE=ON but UE5_OODLE_SDK not set")
    endif()
endif()

# ============================================================================
# Threads (required for bench_multicore.c on Linux/macOS)
# ============================================================================
find_package(Threads REQUIRED)
target_link_libraries(bench PRIVATE Threads::Threads)

# ============================================================================
# Install bench binary
# ============================================================================
install(TARGETS bench RUNTIME DESTINATION bin)
