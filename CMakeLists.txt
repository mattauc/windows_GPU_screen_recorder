cmake_minimum_required(VERSION 3.28)

project(GpuScreenRecorder
    VERSION 0.1.0
    DESCRIPTION "Windows-native GPU screen recorder (ShadowPlay clone)"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR
        "GpuScreenRecorder is Windows-only. Build on Windows with MSVC, or use the "
        "GitHub Actions windows-latest workflow. See BUILDING.md."
    )
endif()

# ---------------------------------------------------------------------------
# Global options
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release configuration for single-config generators
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# MSVC: enable parallel build + reasonable defaults
if(MSVC)
    add_compile_options(/W4 /MP /permissive- /Zc:__cplusplus /utf-8)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00          # Windows 10
        WINVER=0x0A00
    )
endif()

# ---------------------------------------------------------------------------
# Project options
# ---------------------------------------------------------------------------

# Detect NVENC SDK
set(GPUR_NVENC_SDK_PATH "${CMAKE_SOURCE_DIR}/third_party/nvenc_sdk" CACHE PATH
    "Path to NVIDIA Video Codec SDK (must contain Interface/nvEncodeAPI.h)"
)
if(EXISTS "${GPUR_NVENC_SDK_PATH}/Interface/nvEncodeAPI.h")
    set(_gpur_nvenc_default ON)
else()
    set(_gpur_nvenc_default OFF)
endif()

option(GPUR_BUILD_NVENC "Build the NVENC encoder backend"           ${_gpur_nvenc_default})
option(GPUR_BUILD_AMF   "Build the AMD AMF encoder backend"         OFF)
option(GPUR_BUILD_QSV   "Build the Intel Quick Sync backend"        OFF)
option(GPUR_BUILD_TESTS "Build unit tests"                          ON)
option(GPUR_BUILD_UI    "Build the WinUI 3 settings app (Phase 3)"  OFF)

message(STATUS "GpuScreenRecorder configuration:")
message(STATUS "  NVENC backend  : ${GPUR_BUILD_NVENC}")
message(STATUS "  AMF backend    : ${GPUR_BUILD_AMF}")
message(STATUS "  QSV backend    : ${GPUR_BUILD_QSV}")
message(STATUS "  Tests          : ${GPUR_BUILD_TESTS}")
message(STATUS "  UI app         : ${GPUR_BUILD_UI}")
message(STATUS "  NVENC SDK path : ${GPUR_NVENC_SDK_PATH}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(spdlog CONFIG REQUIRED)

if(GPUR_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(daemon)

if(GPUR_BUILD_UI)
    add_subdirectory(ui)
endif()
