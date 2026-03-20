# =============================================================================
# libs/ds/CMakeLists.txt — Build definition for the mk_ds library
# =============================================================================
#
# Header-only data structures (no .cpp files).
# INTERFACE library: no compilation, only propagates include paths and deps.
#
# Dependencies: mk_sys (for sys/bit_utils.hpp used by fixed_hash_map, hash_map, fixed_ring_buffer, ring_buffer)
#
# =============================================================================

add_library(mk_ds INTERFACE)
target_include_directories(mk_ds INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mk_ds INTERFACE mk_sys)
