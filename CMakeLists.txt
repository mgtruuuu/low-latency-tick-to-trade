# =============================================================================
# Root CMakeLists.txt — GlobalHFTPrep
# =============================================================================
#
# This is the top-level build file. CMake reads this first, then follows
# add_subdirectory() calls to discover sub-projects (libraries, tests, etc.).
#
# Build instructions:
#   mkdir build && cd build
#   cmake ..
#   cmake --build .
#
# =============================================================================


# -----------------------------------------------------------------------------
# 1. CMake minimum version and project declaration
# -----------------------------------------------------------------------------
# cmake_minimum_required() must be the first command in the top-level file.
# Version 3.16+ is widely available and supports modern features we use.
#
# project() declares the project name and the language(s) used.
# CXX = C++. This tells CMake to look for a C++ compiler.
#
cmake_minimum_required(VERSION 3.16)
project(GlobalHFTPrep LANGUAGES CXX)

# Target: Linux x86-64 only.
# HFT co-location servers are exclusively x86-64 Linux. This project uses
# x86-specific features (RDTSC, CMPXCHG16B, __int128) and Linux-specific
# syscalls (mmap, epoll, mbind) without cross-platform fallbacks.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project requires Linux (got ${CMAKE_SYSTEM_NAME})")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(FATAL_ERROR "This project requires x86-64 (got ${CMAKE_SYSTEM_PROCESSOR})")
endif()


# -----------------------------------------------------------------------------
# 2. Global compiler settings
# -----------------------------------------------------------------------------
# CMAKE_CXX_STANDARD: which C++ standard to use for ALL targets.
#   We set 20 so features like std::span, concepts, etc. are available.
#
# CMAKE_CXX_STANDARD_REQUIRED: if set to ON, CMake will error out if the
#   compiler doesn't support C++20, rather than silently falling back.
#
# CMAKE_CXX_EXTENSIONS: if set to OFF, disables compiler-specific extensions
#   (e.g., GNU extensions). This means we use strict -std=c++20 instead of
#   -std=gnu++20. Better for portability.
#
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optional clang-tidy integration. When enabled (via dev-tidy preset),
# clang-tidy runs on project targets only — external dependencies
# (GoogleTest, Abseil, Benchmark) are excluded.
option(MK_ENABLE_CLANG_TIDY "Run clang-tidy on project targets" OFF)
if(MK_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy-20 clang-tidy)
    if(CLANG_TIDY_EXE)
        set(MK_CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "MK_ENABLE_CLANG_TIDY is ON but clang-tidy not found")
    endif()
endif()


# -----------------------------------------------------------------------------
# 3. External dependencies (fetched at configure time)
# -----------------------------------------------------------------------------
# FetchContent is a built-in CMake module that downloads and builds external
# projects as part of our build. No separate install step needed — everything
# stays inside the build directory.
#
# GoogleTest: C++ unit-testing framework.
#   GIT_SHALLOW ON  — clone only the latest commit (faster).
#   We pin to a specific release tag for reproducibility.
#
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(googletest)

# Abseil (Swiss Table flat_hash_map, hashing utilities, etc.)
#   ABSL_PROPAGATE_CXX_STD: makes Abseil inherit our CMAKE_CXX_STANDARD (C++20)
#   instead of setting its own. Without this, Abseil may compile with a different
#   standard and cause ODR violations or link errors.
FetchContent_Declare(
    abseil-cpp
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        20240722.0
    GIT_SHALLOW    ON
)
set(ABSL_PROPAGATE_CXX_STD ON)
FetchContent_MakeAvailable(abseil-cpp)

# Google Benchmark: microbenchmark framework.
#   BENCHMARK_ENABLE_TESTING=OFF  — don't build benchmark's own tests.
#   BENCHMARK_ENABLE_GTEST_TESTS=OFF — don't need gtest integration for benchmark.
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    ON
)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
FetchContent_MakeAvailable(benchmark)


# -----------------------------------------------------------------------------
# 4. Add subdirectories (libraries and tests)
# -----------------------------------------------------------------------------
# add_subdirectory() tells CMake to enter a directory and process its
# CMakeLists.txt. libs/ and tests/ each have their own CMakeLists.txt
# that registers individual subdirectories.
#
add_subdirectory(libs)
add_subdirectory(apps)
add_subdirectory(bench)


# -----------------------------------------------------------------------------
# 5. Enable CTest and add tests
# -----------------------------------------------------------------------------
# enable_testing() activates CMake's built-in test runner (CTest).
# This must be called BEFORE add_subdirectory(tests) so that add_test()
# and gtest_discover_tests() inside subdirectories are registered.
#
# include(GoogleTest) loads the GoogleTest CMake module, which provides
# gtest_discover_tests() — a function that queries each gtest binary for
# its test cases and registers them individually with CTest.
#
# After building, run all tests with:
#   ctest --test-dir build/dev --output-on-failure
#
enable_testing()
include(GoogleTest)

add_subdirectory(tests)
