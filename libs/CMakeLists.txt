# =============================================================================
# libs/CMakeLists.txt — Library registration
# =============================================================================
#
# Each subdirectory defines a library target:
#   mk_sys  — compiled library (has .cpp files)
#   mk_ds   — INTERFACE (header-only data structures, depends on mk_sys)
#   mk_net  — INTERFACE (header-only networking, depends on mk_sys)
#   mk_algo — INTERFACE (header-only algorithms, depends on mk_ds)
#
# =============================================================================

add_subdirectory(sys)
add_subdirectory(ds)
add_subdirectory(net)
add_subdirectory(algo)
