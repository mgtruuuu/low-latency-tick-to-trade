/**
 * @file affinity.hpp
 * @brief Thread-to-core pinning (CPU affinity) for Linux.
 *
 * In HFT, threads handling market data or order execution must be pinned
 * to specific CPU cores. Without pinning, the OS scheduler freely migrates
 * threads between cores, causing:
 *
 *   1. L1/L2 cache invalidation (cold cache on the new core)
 *   2. Unpredictable latency spikes from context switches
 *   3. Cross-NUMA memory access on multi-socket servers
 *
 * Pinning a thread tells the OS: "run this thread ONLY on core N".
 * The thread stays on that core, keeping its cache warm and latency
 * deterministic.
 *
 * Typical HFT core assignment:
 *   Core 0     — OS / housekeeping (never pin trading threads here)
 *   Core 1     — Market data receiver
 *   Core 2     — Strategy / signal generation
 *   Core 3     — Order entry / gateway
 *   Core 4+    — Logging, monitoring, etc.
 *
 * Linux API used: pthread_setaffinity_np(), pthread_getaffinity_np()
 * Requires linking with -lpthread (already linked via Threads::Threads).
 */

#pragma once

#include <cstdint>

namespace mk::sys::thread {

/**
 * @brief Pins the calling thread to the specified CPU core.
 *
 * After this call, the OS will only schedule this thread on the given core.
 * Uses pthread_setaffinity_np() internally.
 *
 * @param core_id Zero-based CPU core index (e.g., 0, 1, 2, ...).
 * @return 0 on success, or a positive errno value on failure.
 *         Common errors:
 *           EINVAL — core_id exceeds the number of available cores.
 *           ESRCH  — thread does not exist (should not happen for self).
 *
 * Usage:
 *   int err = mk::sys::thread::pin_current_thread(2);
 *   if (err != 0) { // handle error }
 */
int pin_current_thread(std::uint32_t core_id) noexcept;

/**
 * @brief Returns the CPU core the calling thread is currently running on.
 *
 * Uses sched_getcpu() internally. This is a snapshot — without pinning,
 * the OS may migrate the thread to a different core at any time.
 *
 * @return Core index (>= 0) on success, or -1 on failure.
 */
int get_current_core() noexcept;

/**
 * @brief Returns the number of online CPU cores available to the process.
 *
 * Uses sched_getaffinity() to query the process-level CPU mask.
 * This respects cgroup/container CPU limits (e.g., Docker --cpuset-cpus).
 *
 * @return Number of available cores, or 0 on failure.
 */
std::uint32_t get_available_cores() noexcept;

/**
 * @brief Returns the NUMA node of the specified CPU core.
 *
 * Reads /sys/devices/system/cpu/cpu<N>/node<M> from sysfs.
 * This reflects Linux kernel CPU-to-NUMA topology directly.
 *
 * @param core_id Zero-based CPU core index.
 * @return NUMA node (>= 0) on success, or -1 if sysfs is unavailable.
 */
int get_cpu_numa_node(std::uint32_t core_id) noexcept;

/**
 * @brief Returns the NUMA node of a network interface.
 *
 * Reads /sys/class/net/<iface>/device/numa_node. On single-socket systems
 * or virtual NICs, this returns -1 (kernel reports -1 for non-NUMA devices).
 *
 * Useful for verifying that the trading thread is pinned to the same NUMA
 * node as the NIC, reducing cross-node memory access penalties.
 *
 * @param iface Network interface name (e.g., "eth0", "ens3").
 * @return NUMA node (>= 0) on success, or -1 if unavailable.
 */
int get_nic_numa_node(const char *iface) noexcept;

} // namespace mk::sys::thread
