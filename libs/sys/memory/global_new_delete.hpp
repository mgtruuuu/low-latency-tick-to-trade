/**
 * @file global_new_delete.hpp
 * @brief Global Memory Allocation Overrides Interface
 *
 * This header serves two purposes:
 * 1. Ensures standard headers (<new>) are available.
 * 2. Provides a "Linker Anchor" to force the linker to include the
 * global overrides even when linking from a static library.
 */

#pragma once

namespace mk::sys::memory {

/**
 * @brief Installs the Global Memory Guard.
 *
 * Effectively a no-op at runtime, but calling this function in main()
 * guarantees that the linker does not discard the 'global_new_delete.cpp'
 * object file when linking against static libraries.
 *
 * When building with sanitizers (MK_SANITIZER_ACTIVE), global_new_delete.cpp
 * is excluded from the build to avoid conflicting with sanitizer runtimes'
 * own operator new/delete overrides. In that case this becomes an inline
 * no-op so call sites don't need #ifdef guards.
 *
 * Usage:
 * int main() {
 *   mk::sys::memory::install_global_memory_guard();
 * // ...
 * }
 */
#ifdef MK_SANITIZER_ACTIVE
inline void install_global_memory_guard() {}
#else
void install_global_memory_guard();
#endif

} // namespace mk::sys::memory