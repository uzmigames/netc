# cmake/FetchUnity.cmake
# Downloads the Unity C test framework (ThrowTheSwitch/Unity) single-file release.
# Called by the fetch-unity custom target in CMakeLists.txt.
#
# Usage: cmake -DOUTDIR=<path> -P cmake/FetchUnity.cmake

if(NOT DEFINED OUTDIR)
    message(FATAL_ERROR "OUTDIR must be defined")
endif()

set(UNITY_TAG "v2.6.0")
set(BASE_URL "https://raw.githubusercontent.com/ThrowTheSwitch/Unity/${UNITY_TAG}/src")

foreach(f unity.c unity.h unity_internals.h)
    file(DOWNLOAD
        "${BASE_URL}/${f}"
        "${OUTDIR}/${f}"
        SHOW_PROGRESS
        STATUS status
    )
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        list(GET status 1 msg)
        message(FATAL_ERROR "Failed to download ${f}: ${msg}")
    endif()
    message(STATUS "Downloaded: ${f}")
endforeach()

message(STATUS "Unity ${UNITY_TAG} downloaded to ${OUTDIR}")
