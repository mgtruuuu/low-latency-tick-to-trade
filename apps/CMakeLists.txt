# =============================================================================
# apps/ — Application binaries using libs/ components
# =============================================================================
#
# Two applications:
#   1. tick_to_trade      — Single-threaded trading pipeline
#   2. simulated_exchange — Fake exchange for end-to-end testing
#
# Shared protocol definitions (header-only) are in shared/ and exposed
# via the mk_app_shared INTERFACE library.
#

# Header-only shared protocol (included by both apps)
add_library(mk_app_shared INTERFACE)
target_include_directories(mk_app_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(tick_to_trade)
add_subdirectory(simulated_exchange)
