/**
 * @file lock_free_stack.hpp
 * @brief Lock-Free Stack (Treiber Stack) Implementation using 128-bit CAS.
 *
 * This file defines a lock-free LIFO stack (Treiber Stack) using
 * double-width Compare-And-Swap (CAS) operations to ensure thread safety
 * without locks. It is designed for use in high-performance scenarios,
 * such as object pools, where nodes are pre-allocated and recycled.
 *
 * Key Features:
 * - ABA Protection: Uses a tagged pointer (version counter).
 * - Cache Friendly: Head is aligned to prevent false sharing.
 * - Zero Allocation: Push/Pop operations manipulate existing pointers only.
 */

#pragma once

#include "sys/hardware_constants.hpp"
#include <atomic>
#include <cstdint>

namespace mk::sys::memory {

/**
 * @class LockFreeStack
 * @brief A lock-free LIFO stack (Treiber Stack) using 128-bit
 * CAS(Compare-And-Swap).
 * Designed specifically for Object Pools where nodes are
 * pre-allocated and recycled, rather than allocated/deallocated on the fly.
 *
 * Key Features:
 * - ABA Protection: Uses a tagged pointer (version counter).
 * - Cache Friendly: Head is aligned to prevent false sharing.
 * - Zero Allocation: Push/Pop operations manipulate existing pointers only.
 * @tparam T The type of data to store in the node.
 *
 *
 * @note
 * =============================================================================
 * [Design Note: Intrusive vs. Non-Intrusive Containers]
 * =============================================================================
 * Current Design: Non-Intrusive (Wrapper Style)
 * ---------------------------------------------
 * We wrap the user data 'T' inside a 'Node' struct:
 * Memory Layout: [ Node* next ] [ T data ]
 * - Pros: Decouples 'T' from the stack logic. Works with any type (int,
 * struct, etc.).
 * - Cons: Requires a wrapper 'Node'. When used in an Object Pool, converting
 * T* back to Node* requires pointer arithmetic (reinterpret_cast - offsetof).
 *
 * Future Optimization: Intrusive Design (HFT Preferred)
 * ----------------------------------------------------
 * Embed the 'next' pointer directly inside 'T' (e.g., T inherits from a Hook).
 * Memory Layout: [ T { Node* next; ... members ... } ]
 * - Pros: Zero wrapper overhead. The object IS the node.
 * Simplifies pointer handling (no offset calculations needed).
 * Slightly better cache locality (fewer indirections).
 * - Cons: 'T' must be modified to contain the link pointer.
 * Implementation is more complex (requires template constraints).
 *
 * Decision:
 * We use the Wrapper style for now to keep the implementation simple and
 * generic. The intrusive variant is implemented in
 * intrusive_lock_free_stack.hpp (LockFreeStackHook + IntrusiveLockFreeStack).
 * =============================================================================
 */
template <typename T> class LockFreeStack {
public:
  /**
   * @struct Node
   * @brief The container for the user data.
   * Defined as 'public' so the ObjectPool can allocate a contiguous
   * block of these nodes. We do NOT align this struct individually
   * to keep the memory footprint small (dense packing).
   */
  struct Node {
    Node *next = nullptr; // Pointer to the next node in the stack
    T data;     // The actual user object
  };

private:
  /**
   * @struct TaggedPtr
   * @brief Structure required for double-width CAS (CMPXCHG16B).
   * It combines the pointer and a version tag to solve the ABA problem.
   * Must be aligned to kDoubleWidthAlignment (16 bytes) to allow
   * the CPU to perform atomic 128-bit operations.
   */
  struct alignas(kDoubleWidthAlignment) TaggedPtr {
    Node *ptr{nullptr};
    std::uint64_t tag{0};
  };

  // Compile-time verification for hardware support
  static_assert(sizeof(TaggedPtr) == kDoubleWidthAlignment,
                "TaggedPtr size must match the hardware requirement for "
                "128-bit atomics.");
  static_assert(
      std::atomic<TaggedPtr>::is_always_lock_free,
      "Platform must support lock-free 16-byte atomics (CMPXCHG16B).");

  /**
   * @brief The head of the stack.
   * Aligned to kCacheLineSize (usually 64 bytes) to ensure this atomic
   * variable sits on its own cache line. This prevents "False Sharing" if
   * multiple stacks are adjacent in memory.
   */
  alignas(kCacheLineSize) std::atomic<TaggedPtr> head_;

public:
  // Type alias for external usage
  using NodeType = Node;

  LockFreeStack() noexcept {
    // Initialize with a null pointer and tag 0
    head_.store(TaggedPtr{nullptr, 0}, std::memory_order_relaxed);
  }

  ~LockFreeStack() = default;

  // Non-copyable, non-movable (contains an atomic with address-dependent
  // state).
  LockFreeStack(const LockFreeStack &) = delete;
  LockFreeStack &operator=(const LockFreeStack &) = delete;
  LockFreeStack(LockFreeStack &&) = delete;
  LockFreeStack &operator=(LockFreeStack &&) = delete;

  /**
   * @brief Pushes a node onto the stack.
   * This operation is lock-free and thread-safe.
   * @param node A pointer to the node to be pushed.
   * The caller (ObjectPool) owns the memory of this node.
   */
  void push(Node *node) noexcept {
    TaggedPtr old_head = head_.load(std::memory_order_relaxed);
    TaggedPtr new_head;

    do {
      new_head.ptr = node;
      // Increment the tag to distinguish this pointer version (ABA protection)
      new_head.tag = old_head.tag + 1;

      // Link the new node to the current head
      node->next = old_head.ptr;

      // compare_exchange_weak is sufficient and faster for loops.
      //
      // Success — release: Ensures the write to node->next (line above)
      //   is visible to the thread that later pops this node with acquire.
      //
      // Failure — relaxed is safe here (unlike pop):
      //   On retry we only COPY old_head.ptr into node->next.
      //   We never DEREFERENCE old_head.ptr, so we don't need to see
      //   any data written by another thread. The pointer value itself
      //   is guaranteed correct by the atomic CAS regardless of ordering.
    } while (!head_.compare_exchange_weak(old_head, new_head,
                                          std::memory_order_release,
                                          std::memory_order_relaxed));
  }

  /**
   * @brief Attempts to pop a node from the stack.
   * @return Pointer to the popped node, or nullptr if the stack is empty.
   */
  Node *try_pop() noexcept {
    TaggedPtr old_head = head_.load(std::memory_order_acquire);
    TaggedPtr new_head;

    do {
      // Check for empty stack
      if (old_head.ptr == nullptr) {
        return nullptr;
      }

      // Dereferencing old_head.ptr is safe here ONLY because we are in an
      // Object Pool scenario where nodes are never freed to the OS
      // while other threads might be accessing them.
      new_head.ptr = old_head.ptr->next;

      // Increment tag to maintain global versioning
      new_head.tag = old_head.tag + 1;

      // Success — acquire: Synchronizes with push's release, so we see
      //   the correct node->next value written by the pusher.
      //
      // Failure — acquire (NOT relaxed!):
      //   On retry we DEREFERENCE old_head.ptr->next (line above).
      //   Without acquire, there is no synchronization with the push
      //   that wrote that next pointer. On x86 (TSO) this is masked,
      //   but on ARM64 (LDXR vs LDAXR) it is a real data race.
    } while (!head_.compare_exchange_weak(old_head, new_head,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire));

    return old_head.ptr;
  }

  /**
   * @brief Checks if the stack is empty.
   * Note: The result is only a snapshot and may change immediately in
   * concurrent environments.
   */
  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_relaxed).ptr == nullptr;
  }
};

} // namespace mk::sys::memory