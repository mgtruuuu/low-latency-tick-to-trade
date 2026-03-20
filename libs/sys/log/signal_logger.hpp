#pragma once

#include <cerrno>
#include <charconv> // for std::to_chars, std::errc
#include <cstddef>
#include <cstdint>
#include <cstring> // for std::memcpy
#include <string_view>
#include <type_traits>

#include <unistd.h>

namespace mk::sys::log {

// Standard POSIX file descriptors
// STDERR is preferred for logging because it is typically unbuffered.
constexpr int kFdStdout = 1;
constexpr int kFdStderr = 2;

/**
 * @brief Raw wrapper around write(2). Signal-safe, handles EINTR.
 * Guaranteed no heap allocation.
 */
inline void sys_write(int fd, const char *data, std::size_t len) noexcept {
  const char *ptr = data;
  std::size_t remaining = len;

  while (remaining > 0) {
    auto written = ::write(fd, ptr, remaining);

    if (written < 0) {
      if (errno == EINTR) {
        continue; // System call interrupted by signal, retry immediately
      }
      return; // Fatal error or closed stream, stop trying
    }

    ptr += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

// -----------------------------------------------------------------------------
// Stack buffer — accumulates formatted output for single write()
// No heap allocation, signal-safe.
// -----------------------------------------------------------------------------

struct Buf {
  char *data = nullptr;
  std::size_t cap = 0;
  std::size_t pos = 0;

  std::size_t remaining() const noexcept { return cap - pos; }

  void put(char c) noexcept {
    if (pos < cap) {
      data[pos++] = c;
    }
  }

  void put(std::string_view sv) noexcept {
    const std::size_t n = (sv.size() <= remaining()) ? sv.size() : remaining();
    std::memcpy(data + pos, sv.data(), n);
    pos += n;
  }
};

// -----------------------------------------------------------------------------
// Type-dispatched append into Buf
// -----------------------------------------------------------------------------

/**
 * @brief Appends a value to the buffer. Dispatches by type at compile time.
 *
 * Arguments are passed by value — primitive types (int, double, pointers,
 * string_view) fit in registers and are faster to pass by value than by
 * reference.
 */
template <typename T> inline void append(Buf &buf, T val) noexcept {
  // Prevent accidental std::string copy.
  // If you want to log std::string, cast it to string_view explicitly.
  static_assert(!std::is_class_v<T> || std::is_same_v<T, std::string_view>,
                "HFT Safety: Do not pass objects (like std::string) by value. "
                "Use std::string_view.");

  if constexpr (std::is_same_v<T, bool>) {
    buf.put(val ? '1' : '0');
  } else if constexpr (std::is_same_v<T, char>) {
    buf.put(val);
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    buf.put(val);
  }
  // const char* or char* (arrays decay to pointers here automatically)
  else if constexpr (std::is_pointer_v<T> &&
                     std::is_same_v<
                         std::remove_const_t<std::remove_pointer_t<T>>, char>) {
    if (val) {
      buf.put(val);
    }
  } else if constexpr (std::is_pointer_v<T>) {
    buf.put("0x");
    char tmp[16];
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp),
                                   reinterpret_cast<std::uintptr_t>(val), 16);
    if (ec == std::errc{}) {
      buf.put({tmp, static_cast<std::size_t>(ptr - tmp)});
    }
  } else if constexpr (std::is_floating_point_v<T>) {
    char tmp[64];
    auto [ptr, ec] =
        std::to_chars(tmp, tmp + sizeof(tmp), static_cast<double>(val),
                      std::chars_format::general);
    if (ec == std::errc{}) {
      buf.put({tmp, static_cast<std::size_t>(ptr - tmp)});
    }
  } else if constexpr (std::is_integral_v<T>) {
    char tmp[21]; // enough for int64_min (-9223372036854775808)
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), val);
    if (ec == std::errc{}) {
      buf.put({tmp, static_cast<std::size_t>(ptr - tmp)});
    }
  }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Variadic log to specified fd. Single write() call.
 * Signal-safe, no heap allocation. Truncates at BufSize.
 *
 * Usage: signal_log_to(fd, "Price: ", 3.14, " Count: ", 10, '\n');
 */
template <std::size_t BufSize = 512, typename... Args>
inline void signal_log_to(int fd, Args... args) noexcept {
  char storage[BufSize];
  Buf buf{storage, BufSize};
  (append(buf, args), ...);
  sys_write(fd, buf.data, buf.pos);
}

/**
 * @brief Variadic log to stderr. Single write() call.
 * Usage: signal_log("Price: ", 3.14, " Count: ", 10, '\n');
 */
template <typename... Args> inline void signal_log(Args... args) noexcept {
  signal_log_to(kFdStderr, args...);
}

} // namespace mk::sys::log
