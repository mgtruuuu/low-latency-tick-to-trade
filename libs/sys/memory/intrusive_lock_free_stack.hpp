/**
 * @file intrusive_lock_free_stack.hpp
 * @brief Intrusive Lock-Free Stack (Treiber Stack) using 128-bit CAS.
 *
 * Intrusive variant of lock_free_stack.hpp: the user type T inherits from
 * LockFreeStackHook, embedding the `next` pointer directly in the object.
 * No wrapper Node — T* IS the node pointer.
 *
 * See lock_free_stack.hpp for the non-intrusive (wrapper) variant and the
 * design note comparing both approaches (lines 39-64).
 *
 * Trade-off vs. non-intrusive LockFreeStack<T>:
 *   - Pro: No pointer arithmetic (offsetof/reinterpret_cast) to recover
 *     the node from a T*. T* works directly in the stack, lookup maps,
 *     and queues without conversion.
 *   - Pro: Slightly better cache locality (no wrapper indirection).
 *   - Con: T must inherit from LockFreeStackHook — not usable with
 *     arbitrary types (int, POD, third-party types).
 *
 * Key Features (same as non-intrusive variant):
 *   - ABA Protection: Tagged pointer (version counter) with 128-bit CAS.
 *   - Cache Friendly: Head aligned to cache line boundary.
 *   - Zero Allocation: Push/Pop only manipulate existing pointers.
 */

#pragma once

#include "sys/hardware_constants.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace mk::sys::memory {

// =============================================================================
// LockFreeStackHook — base class for intrusive stack nodes
// =============================================================================
//
// Analogous to IntrusiveListHook (doubly-linked, prev/next), but for a
// singly-linked stack — only a `next` pointer is needed.
//
// Usage: inherit from this hook in your type.
//   struct Order : LockFreeStackHook {
//     uint64_t order_id;
//     int qty;
//   };

struct LockFreeStackHook {
  LockFreeStackHook *next{nullptr};
};

// =============================================================================
// IntrusiveLockFreeStack
// =============================================================================
//
// A lock-free LIFO stack where T inherits from LockFreeStackHook.
// Internally, the TaggedPtr stores LockFreeStackHook* (base pointer).
// Casting to/from T* happens at the API boundary — consistent with how
// IntrusiveList operates on IntrusiveListHook* internally and casts to T*
// at the public interface.

template <typename T> class IntrusiveLockFreeStack {
  static_assert(std::is_base_of_v<LockFreeStackHook, T>,
                "T must inherit from LockFreeStackHook");

  // ---------------------------------------------------------------------------
  // TaggedPtr — 128-bit structure for double-width CAS (CMPXCHG16B)
  // ---------------------------------------------------------------------------
  // Combines a hook pointer with a monotonic version tag to solve ABA.
  // Must be aligned to kDoubleWidthAlignment (16 bytes) for hardware atomics.

  struct alignas(kDoubleWidthAlignment) TaggedPtr {
    LockFreeStackHook *ptr{nullptr};
    std::uint64_t tag{0};
  };

  static_assert(sizeof(TaggedPtr) == kDoubleWidthAlignment,
                "TaggedPtr size must match the hardware requirement for "
                "128-bit atomics.");
  static_assert(
      std::atomic<TaggedPtr>::is_always_lock_free,
      "Platform must support lock-free 16-byte atomics (CMPXCHG16B).");

  // Cache-line aligned to prevent false sharing with adjacent data.
  alignas(kCacheLineSize) std::atomic<TaggedPtr> head_;

public:
  IntrusiveLockFreeStack() noexcept {
    head_.store(TaggedPtr{nullptr, 0}, std::memory_order_relaxed);
  }

  ~IntrusiveLockFreeStack() = default;

  // Non-copyable, non-movable (contains an atomic with address-dependent
  // state).
  IntrusiveLockFreeStack(const IntrusiveLockFreeStack &) = delete;
  IntrusiveLockFreeStack &operator=(const IntrusiveLockFreeStack &) = delete;
  IntrusiveLockFreeStack(IntrusiveLockFreeStack &&) = delete;
  IntrusiveLockFreeStack &operator=(IntrusiveLockFreeStack &&) = delete;

  /// Push a node onto the stack. Lock-free, thread-safe.
  /// @param node Pointer to a T that inherits LockFreeStackHook.
  ///             Caller owns the memory; the stack only links it.
  void push(T *node) noexcept {
    // Upcast to hook pointer — implicit via inheritance.
    auto *hook = static_cast<LockFreeStackHook *>(node);

    TaggedPtr old_head = head_.load(std::memory_order_relaxed);
    TaggedPtr new_head;

    do {
      new_head.ptr = hook;
      new_head.tag = old_head.tag + 1;

      // Link the new node to the current head.
      hook->next = old_head.ptr;

      // Success — release: ensures the write to hook->next (above) is
      //   visible to the thread that later pops this node with acquire.
      //
      // Failure — relaxed: on retry we only COPY old_head.ptr into
      //   hook->next. We never DEREFERENCE old_head.ptr, so we don't
      //   need visibility of data written by another thread. The pointer
      //   value itself is correct by CAS atomicity regardless of ordering.
    } while (!head_.compare_exchange_weak(old_head, new_head,
                                          std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  /// Attempt to pop a node from the stack.
  /// @return Pointer to the popped T, or nullptr if the stack is empty.
  T *try_pop() noexcept {
    TaggedPtr old_head = head_.load(std::memory_order_acquire);
    TaggedPtr new_head;

    do {
      if (old_head.ptr == nullptr) {
        return nullptr;
      }

      // Dereferencing old_head.ptr->next is safe ONLY in an object-pool
      // scenario where nodes are never freed to the OS while other threads
      // may still be reading them.
      new_head.ptr = old_head.ptr->next;
      new_head.tag = old_head.tag + 1;

      // Success — acquire: synchronizes with push's release, so we see
      //   the correct hook->next written by the pusher.
      //
      // Failure — acquire (NOT relaxed!): on retry we DEREFERENCE
      //   old_head.ptr->next. Without acquire, there is no synchronization
      //   with the push that wrote that next pointer. On x86 (TSO) this
      //   is masked, but on ARM64 (LDXR vs LDAXR) it is a real data race.
    } while (!head_.compare_exchange_weak(old_head, new_head,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire));

    // Downcast hook pointer back to T*. Safe because T inherits
    // LockFreeStackHook and we only ever push T* into the stack.
    return static_cast<T *>(old_head.ptr);
  }

  /// Check if the stack is empty (snapshot — may change immediately).
  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_relaxed).ptr == nullptr;
  }
};

} // namespace mk::sys::memory
