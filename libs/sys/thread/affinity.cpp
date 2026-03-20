#include "affinity.hpp"

#include <climits>
#include <cerrno>
#include <cstdio>  // snprintf
#include <cstdlib> // atoi
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace mk::sys::thread {

int pin_current_thread(std::uint32_t core_id) noexcept {
  cpu_set_t cpuset;

  // CPU_ZERO: clears all bits in the CPU set (no cores selected).
  CPU_ZERO(&cpuset);

  // CPU_SET: sets the bit for core_id (allows this core only).
  CPU_SET(core_id, &cpuset);

  // pthread_setaffinity_np: applies the CPU mask to the calling thread.
  //   pthread_self() — handle for the calling thread.
  //   sizeof(cpuset) — size of the cpu_set_t structure.
  //   &cpuset        — pointer to the CPU set.
  //   Returns 0 on success, or a positive errno on failure.
  return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

int get_current_core() noexcept {
  // sched_getcpu: returns the CPU core number the calling thread
  // is currently executing on. Returns -1 on failure (rare).
  return sched_getcpu();
}

std::uint32_t get_available_cores() noexcept {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  // sched_getaffinity: gets the CPU affinity mask for the calling process.
  //   0 — pid 0 means "this process".
  //   Returns 0 on success, -1 on failure.
  if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0) {
    return 0;
  }

  // CPU_COUNT: counts the number of set bits in the CPU mask.
  return static_cast<std::uint32_t>(CPU_COUNT(&cpuset));
}

namespace {

/// Read an integer from a sysfs file. Returns -1 on any failure.
int read_sysfs_int(const char *path) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }

  char buf[32];
  const auto n = ::read(fd, buf, sizeof(buf) - 1);
  const auto saved = errno;
  ::close(fd);
  errno = saved;

  if (n <= 0) {
    return -1;
  }
  buf[n] = '\0';
  return std::atoi(buf);
}

[[nodiscard]] int parse_node_entry(const char *entry_name) noexcept {
  // Expect "node<N>" where N is a non-negative integer.
  if (entry_name == nullptr || entry_name[0] != 'n' || entry_name[1] != 'o' ||
      entry_name[2] != 'd' || entry_name[3] != 'e' || entry_name[4] == '\0') {
    return -1;
  }

  char *end = nullptr;
  errno = 0;
  const long node = std::strtol(entry_name + 4, &end, 10);
  if (errno != 0 || end == entry_name + 4 || *end != '\0' || node < 0 ||
      node > INT_MAX) {
    return -1;
  }
  return static_cast<int>(node);
}

[[nodiscard]] int read_cpu_numa_node_from_sysfs(std::uint32_t core_id) noexcept {
  // /sys/devices/system/cpu/cpu<N>/node<M>
  // Kernel exposes the owning NUMA node as a "node*" entry.
  char cpu_dir_path[128];
  std::snprintf(cpu_dir_path, sizeof(cpu_dir_path),
                "/sys/devices/system/cpu/cpu%u", core_id);

  DIR *dir = ::opendir(cpu_dir_path);
  if (dir == nullptr) {
    return -1;
  }

  int best_node = -1;
  for (dirent *entry = ::readdir(dir); entry != nullptr;
       entry = ::readdir(dir)) {
    const int parsed = parse_node_entry(entry->d_name);
    if (parsed < 0) {
      continue;
    }
    if (best_node < 0 || parsed < best_node) {
      best_node = parsed;
    }
  }
  (void)::closedir(dir);
  return best_node;
}

} // namespace

int get_cpu_numa_node(std::uint32_t core_id) noexcept {
  return read_cpu_numa_node_from_sysfs(core_id);
}

int get_nic_numa_node(const char *iface) noexcept {
  if (iface == nullptr || iface[0] == '\0') {
    return -1;
  }

  // /sys/class/net/<iface>/device/numa_node
  // Kernel returns -1 for non-NUMA or virtual devices.
  char path[256];
  std::snprintf(path, sizeof(path), "/sys/class/net/%s/device/numa_node",
                iface);
  return read_sysfs_int(path);
}

} // namespace mk::sys::thread
