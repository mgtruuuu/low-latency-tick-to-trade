#pragma once

namespace mk::sys::thread {

/**
 * @brief Sets the Hot Path mode for the CURRENT THREAD only.
 * - In Debug builds: If active, 'new' calls will abort the program.
 * - In Release builds: This function does nothing (Zero overhead).
 * @param active true to forbid allocations on this thread.
 */
void set_hot_path_mode(bool active) noexcept;

/**
 * @brief Checks if the current thread is in Hot Path mode.
 */
bool is_hot_path_mode() noexcept;

} // namespace mk::sys::thread