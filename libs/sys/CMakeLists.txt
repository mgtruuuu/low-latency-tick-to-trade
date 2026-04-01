# =============================================================================
# libs/sys/CMakeLists.txt — Build definition for the mk_sys library
# =============================================================================
#
# This file tells CMake how to build the "mk_sys" library target.
# Three key things are defined here:
#   1. WHAT to compile   (source files)
#   2. WHERE to find headers (include paths)
#   3. WHAT to link against (system libraries)
#
# File inventory of libs/sys/:
#
#   Header-only (no .cpp, no compilation needed):
#     sys/hardware_constants.hpp
#     sys/nano_clock.hpp
#     sys/log/signal_logger.hpp
#     sys/memory/mmap_region.hpp  (declarations + inline trivials)
#
#   Compiled (have .cpp, must be listed as sources):
#     sys/memory/global_new_delete.cpp
#     sys/memory/mmap_region.cpp
#     sys/thread/hot_path_control.cpp
#     sys/thread/affinity.cpp
#
# =============================================================================


# -----------------------------------------------------------------------------
# 1. Define the library target and list source files
# -----------------------------------------------------------------------------
# add_library(<name> <source files...>)
#
#   Creates a library target named "mk_sys".
#   Only .cpp files need to be listed here — they get compiled into object files
#   and linked together into a static library (libmk_sys.a by default).
#
#   Header-only files (signal_logger.hpp, hardware_constants.hpp, etc.) are NOT listed here.
#   They don't need to be compiled — they are included (#include) by other files.
#   CMake only needs to know WHERE to find them (see section 2 below).
#
# Sanitizer runtimes (ASan, TSan) provide their own operator new/delete
# overrides. Including global_new_delete.cpp causes multiple-definition
# linker errors. Exclude it when any sanitizer is active, and define
# MK_SANITIZER_ACTIVE so the header provides an inline no-op stub.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
    set(MK_SYS_SOURCES
        memory/mmap_utils.cpp
        memory/mmap_region.cpp
        thread/hot_path_control.cpp
        thread/affinity.cpp
    )
else()
    set(MK_SYS_SOURCES
        memory/global_new_delete.cpp
        memory/mmap_utils.cpp
        memory/mmap_region.cpp
        thread/hot_path_control.cpp
        thread/affinity.cpp
    )
endif()

add_library(mk_sys ${MK_SYS_SOURCES})

# Internal-only headers (under libs/sys/private) require this define.
target_compile_definitions(mk_sys PRIVATE MK_SYS_INTERNAL_API)

if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
    target_compile_definitions(mk_sys PUBLIC MK_SANITIZER_ACTIVE)
endif()


# -----------------------------------------------------------------------------
# 2. Set the include search path
# -----------------------------------------------------------------------------
# target_include_directories(<target> <PUBLIC|PRIVATE> <paths...>)
#
#   Tells the compiler where to search when it sees #include "...".
#
#   We set the path to the parent directory: libs/sys/.. = libs/
#   This means #include "sys/log/safe_logger.hpp" resolves to:
#     libs/ + sys/log/safe_logger.hpp = libs/sys/log/safe_logger.hpp
#
#   PUBLIC vs PRIVATE:
#     PUBLIC  = this path is used by mk_sys itself AND anyone who links to mk_sys.
#     PRIVATE = this path is only used by mk_sys itself.
#
#   We use PUBLIC because any target that links mk_sys (e.g., a test executable)
#   also needs to find headers like #include "sys/log/safe_logger.hpp".
#
#   ${CMAKE_CURRENT_SOURCE_DIR} is a CMake built-in variable that always points
#   to the directory containing this CMakeLists.txt file (= libs/sys/).
#
target_include_directories(mk_sys
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/private
)


# -----------------------------------------------------------------------------
# 3. Link system libraries
# -----------------------------------------------------------------------------
# target_link_libraries(<target> <PUBLIC|PRIVATE> <libraries...>)
#
#   Tells the linker which external libraries to link against.
#
#   Threads::Threads is CMake's portable way to link the pthreads library.
#   It is needed because:
#     - thread/hot_path_control.cpp uses pthread functions (cpu affinity, etc.)
#     - On Linux this adds -lpthread to the linker command.
#
#   find_package(Threads REQUIRED) searches for the threading library on the
#   system and makes the Threads::Threads target available.
#
#   PUBLIC means any target linking mk_sys also gets -lpthread automatically.
#
find_package(Threads REQUIRED)
target_link_libraries(mk_sys PUBLIC Threads::Threads)


# -----------------------------------------------------------------------------
# 4. Architecture-specific flags
# -----------------------------------------------------------------------------
# -mcx16: Enables the CMPXCHG16B instruction (128-bit Compare-And-Swap).
# Required by LockFreeStack's std::atomic<TaggedPtr> (16-byte struct).
# Without this, std::atomic::is_always_lock_free is false and the
# static_assert in lock_free_stack.hpp fails.
#
# PUBLIC because any target including lock_free_stack.hpp (tests, ObjectPool)
# also needs this instruction available.
#
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(mk_sys PUBLIC -mcx16)
endif()


# -----------------------------------------------------------------------------
# 5. Optional clang-tidy integration
# -----------------------------------------------------------------------------
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(mk_sys PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
