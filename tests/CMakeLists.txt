# =============================================================================
# tests/CMakeLists.txt — Test executable registration
# =============================================================================
#
# Helper function to reduce boilerplate when adding tests.
# Each test only needs one line: mk_add_test(<name> <source> <library>)
#
# Adding a new test:
#   mk_add_test(ring_buffer_test  ds/ring_buffer_test.cpp  mk_sys)
#
# =============================================================================


# -----------------------------------------------------------------------------
# Helper function
# -----------------------------------------------------------------------------
# function() defines a reusable CMake command.
#
#   mk_add_test(name source lib)
#     name   — executable name (e.g., signal_logger_test)
#     source — path relative to this directory (e.g., sys/log/signal_logger_test.cpp)
#     lib    — library to link against (e.g., mk_sys)
#
# This creates an executable and links it to the specified library.
# All PUBLIC include paths and link dependencies from the library are
# inherited automatically.
#
function(mk_add_test name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib})
endfunction()


# -----------------------------------------------------------------------------
# Helper function — gtest-based unit tests
# -----------------------------------------------------------------------------
# mk_add_gtest(name source lib)
#   Same interface as mk_add_test, but additionally links GTest::gtest_main
#   so the test gets a main() from gtest and is registered with CTest.
#
#   gtest_discover_tests() queries the compiled binary at build time to
#   discover all TEST/TEST_F cases and registers each one with CTest.
#   This means `ctest` will show individual test results, not just
#   pass/fail per binary.
#
function(mk_add_gtest name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib} GTest::gtest_main)
    gtest_discover_tests(${name})
    if(MK_CLANG_TIDY_COMMAND)
        set_target_properties(${name} PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
    endif()
endfunction()


# -----------------------------------------------------------------------------
# Subdirectories (one per category)
# -----------------------------------------------------------------------------
add_subdirectory(libs)
add_subdirectory(apps)
