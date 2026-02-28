cmake_minimum_required(VERSION 3.20)
project(netc VERSION 0.1.0 LANGUAGES C)

# =============================================================================
# C standard
# =============================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# =============================================================================
# Build options
# =============================================================================
option(NETC_BUILD_TESTS       "Build unit tests"             ON)
option(NETC_BUILD_BENCH       "Build benchmark harness"      ON)
option(NETC_ENABLE_SANITIZERS "Enable ASan + UBSan in Debug" ON)
option(NETC_BENCH_WITH_OODLE  "Enable OodleNetwork adapter"  OFF)
option(NETC_BUILD_CSHARP_SDK  "Build native lib for C# SDK"  OFF)

# =============================================================================
# Compiler warnings and flags
# =============================================================================
if(MSVC)
    add_compile_options(
        /W4
        /WX           # warnings as errors
        /wd4200       # zero-size array in struct (used in some platforms)
    )
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wstrict-aliasing=2
        -Wundef
        -fno-strict-aliasing  # safe memcpy-based type punning
    )
endif()

# =============================================================================
# Optimization flags per build type
# =============================================================================
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_C_FLAGS_DEBUG   "/Od /Zi /RTC1")
else()
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -fomit-frame-pointer")
    set(CMAKE_C_FLAGS_DEBUG   "-O0 -g3")
    if(NETC_ENABLE_SANITIZERS)
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address,undefined")
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address,undefined")
    endif()
endif()

# =============================================================================
# SIMD detection
# =============================================================================
include(CheckCCompilerFlag)

set(NETC_SIMD_FLAGS "")

if(MSVC)
    # MSVC x64: SSE4.2 and AVX2 intrinsics are always available via immintrin.h.
    # Runtime detection (CPUID) happens in netc_simd_generic.c.
    # /arch:AVX2 enables AVX2 code generation; without it MSVC will still compile
    # the intrinsic calls but the optimizer won't auto-vectorize with AVX2.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
        add_compile_definitions(NETC_SIMD_SSE42=1 NETC_SIMD_AVX2=1)
        # /arch:AVX2 — enables AVX2 code generation for the entire library
        list(APPEND NETC_SIMD_FLAGS "/arch:AVX2")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    check_c_compiler_flag("-msse4.2" NETC_HAS_SSE42)
    check_c_compiler_flag("-mavx2"   NETC_HAS_AVX2)
    if(NETC_HAS_AVX2)
        list(APPEND NETC_SIMD_FLAGS "-mavx2")
        add_compile_definitions(NETC_SIMD_AVX2=1 NETC_SIMD_SSE42=1)
    elseif(NETC_HAS_SSE42)
        list(APPEND NETC_SIMD_FLAGS "-msse4.2")
        add_compile_definitions(NETC_SIMD_SSE42=1)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)")
    check_c_compiler_flag("-march=armv8-a+simd" NETC_HAS_NEON)
    if(NETC_HAS_NEON)
        list(APPEND NETC_SIMD_FLAGS "-march=armv8-a+simd+crc")
        add_compile_definitions(NETC_SIMD_NEON=1)
    endif()
endif()

# =============================================================================
# Core library source files
# =============================================================================
set(NETC_CORE_SOURCES
    src/core/netc_ctx.c
    src/core/netc_dict.c
    src/core/netc_compress.c
    src/core/netc_decompress.c
    src/algo/netc_tans.c
    src/util/netc_crc32.c
    src/simd/netc_simd_generic.c
    src/simd/netc_simd_sse42.c
    src/simd/netc_simd_avx2.c
    src/simd/netc_simd_neon.c
)

# =============================================================================
# Static library: netc
# =============================================================================
add_library(netc STATIC ${NETC_CORE_SOURCES})
target_include_directories(netc
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(NETC_SIMD_FLAGS)
    target_compile_options(netc PRIVATE ${NETC_SIMD_FLAGS})
endif()

# =============================================================================
# Shared library: netc_shared (for C# P/Invoke and dynamic linking)
# =============================================================================
add_library(netc_shared SHARED ${NETC_CORE_SOURCES})
target_include_directories(netc_shared
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(netc_shared PROPERTIES OUTPUT_NAME "netc")
if(NETC_SIMD_FLAGS)
    target_compile_options(netc_shared PRIVATE ${NETC_SIMD_FLAGS})
endif()

# =============================================================================
# Tests
# =============================================================================
if(NETC_BUILD_TESTS)
    enable_testing()

    # Download Unity C test framework (single-file, embedded in vendor/)
    set(UNITY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity/unity.c")
    set(UNITY_INC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity")

    if(NOT EXISTS "${UNITY_SRC}")
        message(STATUS "Unity not found — run: cmake --build . --target fetch-unity")
    endif()

    # Helper function for test targets
    function(add_netc_test name)
        add_executable(${name} ${ARGN} ${UNITY_SRC})
        target_include_directories(${name} PRIVATE ${UNITY_INC} include src)
        target_link_libraries(${name} PRIVATE netc)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    endfunction()

    # Test targets
    add_netc_test(test_api        tests/test_api.c)
    add_netc_test(test_passthru   tests/test_passthru.c)
    add_netc_test(test_bitstream  tests/test_bitstream.c)
    add_netc_test(test_dict       tests/test_dict.c)
    add_netc_test(test_compress   tests/test_compress.c)
    add_netc_test(test_tans_debug tests/test_tans_debug.c)
    add_netc_test(test_delta      tests/test_delta.c)
    add_netc_test(test_simd       tests/test_simd.c)
    add_netc_test(test_security   tests/test_security.c)
    add_netc_test(test_compact_header tests/test_compact_header.c)
    add_executable(compress_debug tests/test_compress_debug.c)
    target_include_directories(compress_debug PRIVATE ${UNITY_INC} include src)
    target_link_libraries(compress_debug PRIVATE netc)
endif()

# =============================================================================
# Fuzz targets (clang -fsanitize=fuzzer only — not built by default)
# =============================================================================
option(NETC_BUILD_FUZZ "Build libFuzzer fuzz targets (requires clang)" OFF)
if(NETC_BUILD_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(WARNING "NETC_BUILD_FUZZ=ON requires clang; current compiler: ${CMAKE_C_COMPILER_ID}")
    endif()

    function(add_fuzz_target name src)
        add_executable(${name} ${src})
        target_include_directories(${name} PRIVATE include src)
        target_link_libraries(${name} PRIVATE netc)
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -O1 -g)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endfunction()

    add_fuzz_target(fuzz_decompress tests/fuzz_decompress.c)
    add_fuzz_target(fuzz_dict_load  tests/fuzz_dict_load.c)
    add_fuzz_target(fuzz_compress   tests/fuzz_compress.c)

    message(STATUS "Fuzz targets enabled: fuzz_decompress fuzz_dict_load fuzz_compress")
endif()

# =============================================================================
# Coverage target (gcov / llvm-cov — GCC or clang Debug builds only)
# =============================================================================
option(NETC_ENABLE_COVERAGE "Enable gcov/llvm-cov coverage instrumentation" OFF)
if(NETC_ENABLE_COVERAGE)
    if(MSVC)
        message(WARNING "NETC_ENABLE_COVERAGE not supported on MSVC; use clang or GCC")
    else()
        # Instrument the library and test executables
        target_compile_options(netc PRIVATE --coverage -O0 -g)
        target_link_options(netc PRIVATE --coverage)

        # Coverage report target (after running ctest)
        find_program(LCOV_BIN lcov)
        find_program(GENHTML_BIN genhtml)
        if(LCOV_BIN AND GENHTML_BIN)
            add_custom_target(coverage
                COMMAND ${LCOV_BIN} --directory . --capture --output-file coverage.info
                COMMAND ${LCOV_BIN} --remove coverage.info
                        '*/vendor/*' '*/tests/*' '/usr/*'
                        --output-file coverage_filtered.info
                COMMAND ${GENHTML_BIN} coverage_filtered.info
                        --output-directory coverage_html
                COMMAND ${LCOV_BIN} --summary coverage_filtered.info
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating coverage report → ${CMAKE_BINARY_DIR}/coverage_html/index.html"
                VERBATIM
            )
            message(STATUS "Coverage target: cmake --build . --target coverage")
        else()
            message(STATUS "lcov/genhtml not found; coverage HTML report unavailable")
        endif()
    endif()
endif()

# =============================================================================
# PGO build support (GCC / clang; three-step: instrument → profile → optimized)
# =============================================================================
option(NETC_PGO_INSTRUMENT  "Step 1: build with PGO instrumentation"        OFF)
option(NETC_PGO_OPTIMIZE    "Step 3: build with PGO profile data applied"    OFF)
set(NETC_PGO_DATA_DIR "${CMAKE_BINARY_DIR}/pgo_data" CACHE PATH "PGO profile data directory")

if(NETC_PGO_INSTRUMENT AND NOT MSVC)
    message(STATUS "PGO: instrumentation build — run bench to generate profile data")
    target_compile_options(netc PRIVATE -fprofile-generate=${NETC_PGO_DATA_DIR})
    target_link_options(netc   PRIVATE -fprofile-generate=${NETC_PGO_DATA_DIR})
elseif(NETC_PGO_OPTIMIZE AND NOT MSVC)
    if(NOT EXISTS "${NETC_PGO_DATA_DIR}")
        message(FATAL_ERROR "PGO profile data directory not found: ${NETC_PGO_DATA_DIR}\n"
                "Run an instrumentation build first: cmake -DNETC_PGO_INSTRUMENT=ON .")
    endif()
    message(STATUS "PGO: optimized build using profile data from ${NETC_PGO_DATA_DIR}")
    target_compile_options(netc PRIVATE -fprofile-use=${NETC_PGO_DATA_DIR} -fprofile-correction)
    target_link_options(netc   PRIVATE -fprofile-use=${NETC_PGO_DATA_DIR})
endif()

# =============================================================================
# Benchmark harness (Phase 5)
# =============================================================================
if(NETC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# =============================================================================
# OodleNetwork adapter (optional, requires UE5 SDK)
# =============================================================================
if(NETC_BENCH_WITH_OODLE)
    if(NOT DEFINED UE5_OODLE_SDK AND NOT DEFINED ENV{UE5_OODLE_SDK})
        message(FATAL_ERROR
            "NETC_BENCH_WITH_OODLE=ON requires -DUE5_OODLE_SDK=<path> "
            "pointing to the OodleNetwork SDK directory.\n"
            "Expected: W:/UE_5.6_Source/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.13/")
    endif()
    if(DEFINED ENV{UE5_OODLE_SDK} AND NOT DEFINED UE5_OODLE_SDK)
        set(UE5_OODLE_SDK "$ENV{UE5_OODLE_SDK}")
    endif()
    message(STATUS "OodleNetwork SDK: ${UE5_OODLE_SDK}")
    # bench/bench_oodle.c will be added in Phase 5
endif()

# =============================================================================
# Fetch Unity (custom target, not built automatically)
# =============================================================================
find_package(Python3 QUIET)
if(Python3_FOUND OR UNIX)
    add_custom_target(fetch-unity
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity"
        COMMAND ${CMAKE_COMMAND} -E echo "Downloading Unity test framework..."
        COMMAND ${CMAKE_COMMAND}
                -DOUTDIR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchUnity.cmake
        COMMENT "Fetching Unity C test framework"
        VERBATIM
    )
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS netc netc_shared
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES include/netc.h DESTINATION include)

# =============================================================================
# Summary
# =============================================================================
message(STATUS "")
message(STATUS "netc ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "  C standard:      C${CMAKE_C_STANDARD}")
message(STATUS "  SIMD flags:      ${NETC_SIMD_FLAGS}")
message(STATUS "  Build tests:     ${NETC_BUILD_TESTS}")
message(STATUS "  Build bench:     ${NETC_BUILD_BENCH}")
message(STATUS "  With Oodle:      ${NETC_BENCH_WITH_OODLE}")
message(STATUS "")
