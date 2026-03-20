# =============================================================================
# libs/algo/CMakeLists.txt — Build definition for the mk_algo library
# =============================================================================
#
# Header-only algorithms (no .cpp files).
# INTERFACE library: no compilation, only propagates include paths and deps.
#
# Dependencies: mk_ds (for data structures used by matching engine, etc.)
#
# =============================================================================

add_library(mk_algo INTERFACE)
target_include_directories(mk_algo INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mk_algo INTERFACE mk_ds)
