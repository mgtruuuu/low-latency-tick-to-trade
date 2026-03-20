#include "hot_path_control.hpp"

// Anonymous namespace: Ensures 't_hot_path_active' is visible ONLY within this
// file. This prevents linker collisions with other files.
namespace {
thread_local bool t_hot_path_active = false;
}

namespace mk::sys::thread {

void set_hot_path_mode(bool active) noexcept {
#ifndef NDEBUG
  // Debug Mode: Update the thread-local flag.
  // This allows specific threads (e.g., Trading Thread) to be guarded
  // while others (e.g., Logging Thread) function normally.
  t_hot_path_active = active;
#else
  // Release Mode: Compiler optimizes this away to a no-op.
  // Zero runtime overhead.
  (void)active;
#endif
}

bool is_hot_path_mode() noexcept {
#ifndef NDEBUG
  return t_hot_path_active;
#else
  return false;
#endif
}

} // namespace mk::sys::thread