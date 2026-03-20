# bench/ — Microbenchmarks mirroring libs/ structure.
#
# Two benchmark styles coexist:
#   *_bench.cpp  — Custom rdtsc benchmarks (hot-path precision, percentile reporting)
#   *_gbench.cpp — Google Benchmark (CI-friendly, regression tracking)

# Capture bench root for include path (before add_subdirectory changes scope).
set(BENCH_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Shared infrastructure for custom rdtsc benchmarks (compiled once).
add_library(mk_bench_utils STATIC bench_utils.cpp)
target_link_libraries(mk_bench_utils PUBLIC mk_sys)
target_include_directories(mk_bench_utils PUBLIC ${BENCH_ROOT_DIR})

function(mk_add_bench name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib} mk_bench_utils)
    if(MK_CLANG_TIDY_COMMAND)
        set_target_properties(${name} PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
    endif()
endfunction()

function(mk_add_gbench name source lib)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${lib} benchmark::benchmark benchmark::benchmark_main)
    if(MK_CLANG_TIDY_COMMAND)
        set_target_properties(${name} PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
    endif()
endfunction()

add_subdirectory(libs)
