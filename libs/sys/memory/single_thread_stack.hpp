/**
 * @file single_thread_stack.hpp
 * @brief Single-threaded LIFO stack — zero-synchronization ObjectPool policy.
 *
 * Drop-in replacement for LockFreeStack<T> when the pool is used by exactly
 * one thread (the common HFT hot-path case: thread-per-core architecture).
 *
 * Same Node layout as LockFreeStack<T>::Node — { Node* next; T data; } —
 * so ObjectPool::deallocate() pointer arithmetic (offsetof) works identically
 * regardless of which stack policy is used.
 *
 * Why not just use LockFreeStack on a single thread?
 *   LockFreeStack uses 128-bit CMPXCHG16B for every push/pop. Even on a
 *   single thread, this instruction has higher latency than a simple pointer
 *   store:
 *     - CMPXCHG16B: ~20 cycles (implicit LOCK prefix, bus lock on some CPUs)
 *     - MOV (pointer store): ~1 cycle
 *   Additionally, LockFreeStack's cache-line-aligned atomic head wastes 48
 *   bytes of padding per stack instance. For per-core pools, this adds up.
 *
 * Thread safety: NONE. Caller must ensure single-threaded access.
 * For concurrent pools, use LockFreeStack<T> (the ObjectPool default).
 */

#pragma once

namespace mk::sys::memory {

template <typename T> class SingleThreadStack {
public:
  /// Same Node layout as LockFreeStack<T>::Node.
  /// This is critical: ObjectPool::deallocate() uses offsetof(NodeType, data)
  /// to recover the node pointer from a T*. Both stacks must use the same
  /// layout so the offset is identical.
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
