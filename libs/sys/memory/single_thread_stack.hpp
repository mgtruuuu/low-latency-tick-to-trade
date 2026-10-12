/**
 * @file single_thread_stack.hpp
 * @brief Single-threaded LIFO stack — zero-synchronization Treiber-layout
 * stack.
 *
 * The single-threaded counterpart to LockFreeStack<T>: the same intrusive
 * Node layout — { Node* next; T data; } — but a plain pointer head instead of
 * a 128-bit-CAS atomic. For the common HFT hot-path case where exactly one
 * thread pushes and pops (thread-per-core / shared-nothing architecture).
 *
 * Why not just use LockFreeStack on a single thread?
 *   LockFreeStack uses 128-bit CMPXCHG16B for every push/pop. Even on a
 *   single thread, this instruction has higher latency than a simple pointer
 *   store:
 *     - CMPXCHG16B: ~20 cycles (implicit LOCK prefix, bus lock on some CPUs)
 *     - MOV (pointer store): ~1 cycle
 *   Additionally, LockFreeStack's cache-line-aligned atomic head wastes 48
 *   bytes of padding per stack instance. For per-core data structures, this
 *   adds up.
 *
 * Thread safety: NONE. Caller must ensure single-threaded access.
 * For concurrent (MPMC) use, see LockFreeStack<T>.
 */

#pragma once

namespace mk::sys::memory {

template <typename T> class SingleThreadStack {
public:
  /// Same Node layout as LockFreeStack<T>::Node — { Node* next; T data; }.
  /// An intrusive node carrying the user's T inline; recovering a T* from a
  /// node (and vice versa) is a fixed offsetof(NodeType, data), identical to
  /// LockFreeStack's — kept identical for direct conceptual comparison
  /// (single-threaded store vs 128-bit CAS) and benchmarking.
  struct Node {
    Node *next;
    T data;
  };

  using NodeType = Node;

  SingleThreadStack() noexcept = default;
  ~SingleThreadStack() = default;

  // Non-copyable, non-movable — same semantics as LockFreeStack.
  // Stacks manage linked pointers into externally-owned memory;
  // copying or moving would create dangling or aliased node chains.
  SingleThreadStack(const SingleThreadStack &) = delete;
  SingleThreadStack &operator=(const SingleThreadStack &) = delete;
  SingleThreadStack(SingleThreadStack &&) = delete;
  SingleThreadStack &operator=(SingleThreadStack &&) = delete;

  /// Push a node onto the stack. O(1), zero synchronization.
  void push(Node *node) noexcept {
    node->next = head_;
    head_ = node;
  }

  /// Pop a node from the stack. Returns nullptr if empty. O(1).
  Node *try_pop() noexcept {
    if (head_ == nullptr) [[unlikely]] {
      return nullptr;
    }
    Node *node = head_;
    head_ = head_->next;
    return node;
  }

  /// Check if the stack is empty. Exact (not a snapshot — single-threaded).
  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
  Node *head_{nullptr};
};

} // namespace mk::sys::memory
