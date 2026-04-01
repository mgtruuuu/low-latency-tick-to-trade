/**
 * @file intrusive_list.hpp
 * @brief Intrusive doubly-linked list with sentinel node — zero allocation.
 *
 * Core data structure for HFT hot paths: order book price-level chains,
 * timer wheel buckets, object pool free lists, etc.
 *
 * Why intrusive?
 *   std::list allocates a heap node per element (operator new on every insert).
 *   An intrusive list embeds the prev/next pointers directly inside the user
 *   object. The list never allocates — it only manipulates existing pointers.
 *   The user controls object lifetime and memory layout (arena, pool, stack).
 *
 * Design:
 *   - Hook-based inheritance: user type inherits IntrusiveListHook to embed
 *     prev/next pointers. Same pattern as Boost.Intrusive list_base_hook.
 *   - Circular doubly-linked list with sentinel node. The sentinel eliminates
 *     all nullptr checks in insert/erase — prev and next are always valid.
 *   - Bidirectional iterator via static_cast<T&> downcast from hook pointer.
 *   - Non-copyable. Movable via O(1) sentinel fixup (the first and last
 *     nodes' pointers are patched to the new sentinel address). Moves are
 *     safe at startup but should not be done during hot-path use.
 *
 * Usage:
 *   struct Order : mk::ds::IntrusiveListHook {
 *     std::uint64_t order_id;
 *     int price;
 *     int qty;
 *   };
 *
 *   mk::ds::IntrusiveList<Order> orders;
 *   Order o1{.order_id = 1, .price = 100, .qty = 10};
 *   orders.push_back(o1);   // zero allocation — sets o1.prev/next only
 *   orders.erase(o1);       // O(1) unlink
 *
 * Constraints:
 *   - A node can belong to at most ONE list at a time (single hook).
 *   - The user must ensure nodes outlive the list (or call clear() first).
 *   - Debug asserts catch double-insert and pop-from-empty.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility> // std::swap

namespace mk::ds {

// =============================================================================
// IntrusiveListHook — embed in user type via public inheritance
// =============================================================================

/// Base hook that provides prev/next pointers for intrusive list linkage.
/// A node is "linked" when next != nullptr (i.e., it belongs to a list).
/// When unlinked, both prev and next are nullptr.
struct IntrusiveListHook {
  IntrusiveListHook *prev{nullptr};
  IntrusiveListHook *next{nullptr};

  /// Non-copyable, non-movable: copying a linked node would duplicate
  /// prev/next pointers, corrupting the list. Same design as
  /// Boost.Intrusive's safe_link hooks. Nodes are pre-allocated in pools
  /// and linked/unlinked — never copied.
  IntrusiveListHook() noexcept = default;
  ~IntrusiveListHook() = default;
  IntrusiveListHook(const IntrusiveListHook &) = delete;
  IntrusiveListHook &operator=(const IntrusiveListHook &) = delete;
  IntrusiveListHook(IntrusiveListHook &&) = delete;
  IntrusiveListHook &operator=(IntrusiveListHook &&) = delete;

  /// Returns true if this node is currently linked into a list.
  /// Unlinked nodes have next == nullptr (prev is also nullptr).
  [[nodiscard]] bool is_linked() const noexcept { return next != nullptr; }
};

// =============================================================================
// IntrusiveList<T> — circular doubly-linked list with sentinel
// =============================================================================

template <class T> class IntrusiveList {
  static_assert(std::is_base_of_v<IntrusiveListHook, T>,
                "T must inherit from IntrusiveListHook");

  // ---------------------------------------------------------------------------
  // Data members
  // ---------------------------------------------------------------------------

  // Sentinel node: always present, forms the circular link anchor.
  // In an empty list, sentinel_.next == sentinel_.prev == &sentinel_.
  IntrusiveListHook sentinel_;
  std::size_t size_{0};

public:
  // Forward-declare iterator; definition below.
  template <bool IsConst> class IteratorImpl;

  using Iterator = IteratorImpl<false>;
  using ConstIterator = IteratorImpl<true>;

  IntrusiveList() noexcept { init_sentinel(); }

  ~IntrusiveList() noexcept {
    // Unlink all nodes so is_linked() returns false for each.
    // This is O(n) but necessary for correctness — without it, nodes would
    // still have dangling prev/next pointing at the destroyed sentinel.
    clear();
  }

  // Non-copyable: copying a linked list would require deep-cloning every
  // node, which intrusive lists cannot do (nodes are externally owned).
  IntrusiveList(const IntrusiveList &) = delete;
  IntrusiveList &operator=(const IntrusiveList &) = delete;

  // Movable: O(1) sentinel fixup. After move, source is empty.
  // Safe only before concurrent use — same constraint as HashMap/SPSCQueue.
  //
  // Sentinel fixup: linked nodes point to the sentinel via prev/next.
  // After moving sentinel_ to a new address, the first and last nodes
  // still point to the OLD sentinel. Fix by updating those two pointers.
  IntrusiveList(IntrusiveList &&other) noexcept : size_(other.size_) {
    if (other.size_ == 0) {
      init_sentinel();
    } else {
      sentinel_.prev = other.sentinel_.prev;
      sentinel_.next = other.sentinel_.next;
      sentinel_.next->prev = &sentinel_; // first node → new sentinel
      sentinel_.prev->next = &sentinel_; // last node → new sentinel
      other.init_sentinel();
      other.size_ = 0;
    }
  }

  IntrusiveList &operator=(IntrusiveList &&other) noexcept {
    if (this != &other) {
      IntrusiveList tmp(std::move(other));
      swap(tmp); // tmp destructor clears our old nodes
    }
    return *this;
  }

  void swap(IntrusiveList &other) noexcept {
    // Swap sentinel links and sizes.
    std::swap(sentinel_.prev, other.sentinel_.prev);
    std::swap(sentinel_.next, other.sentinel_.next);
    std::swap(size_, other.size_);
    // After swap, edge nodes still point to the old sentinel. Fix them.
    fix_sentinel_links();
    other.fix_sentinel_links();
  }

  friend void swap(IntrusiveList &a, IntrusiveList &b) noexcept { a.swap(b); }

  // ---------------------------------------------------------------------------
  // Capacity
  // ---------------------------------------------------------------------------

  /// Number of elements in the list. O(1).
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// True if the list contains no elements.
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  // ---------------------------------------------------------------------------
  // Element access
  // ---------------------------------------------------------------------------

  /// Reference to the first element. UB if empty (debug assert).
  [[nodiscard]] T &front() noexcept {
    assert(!empty());
    return static_cast<T &>(*sentinel_.next);
  }

  [[nodiscard]] const T &front() const noexcept {
    assert(!empty());
    return static_cast<const T &>(*sentinel_.next);
  }

  /// Reference to the last element. UB if empty (debug assert).
  [[nodiscard]] T &back() noexcept {
    assert(!empty());
    return static_cast<T &>(*sentinel_.prev);
  }

  [[nodiscard]] const T &back() const noexcept {
    assert(!empty());
    return static_cast<const T &>(*sentinel_.prev);
  }

  // ---------------------------------------------------------------------------
  // Modifiers — all O(1), zero allocation
  // ---------------------------------------------------------------------------

  /// Insert node at the front of the list.
  /// Precondition: node must not already be linked (debug assert).
  void push_front(T &node) noexcept {
    assert(!node.is_linked() && "double insert: node is already linked");
    link_after(&sentinel_, &node);
    ++size_;
  }

  /// Insert node at the back of the list.
  /// Precondition: node must not already be linked (debug assert).
  void push_back(T &node) noexcept {
    assert(!node.is_linked() && "double insert: node is already linked");
    link_before(&sentinel_, &node);
    ++size_;
  }

  /// Remove and return the first element.
  /// Precondition: list must not be empty (debug assert).
  T &pop_front() noexcept {
    assert(!empty());
    T &node = front();
    unlink(&node);
    --size_;
    return node;
  }

  /// Remove and return the last element.
  /// Precondition: list must not be empty (debug assert).
  T &pop_back() noexcept {
    assert(!empty());
    T &node = back();
    unlink(&node);
    --size_;
    return node;
  }

  /// Insert new_node immediately before pos in the list. O(1).
  /// Use case: maintaining a sorted list — walk to the insertion point,
  /// then insert_before the first element that compares greater/less.
  ///
  /// Preconditions:
  ///   - pos must be linked (belong to this list).
  ///   - new_node must NOT be linked.
  void insert_before(T &pos, T &new_node) noexcept {
    assert(pos.is_linked() && "insert_before: pos is not linked");
    assert(!new_node.is_linked() &&
           "insert_before: new_node is already linked");
    link_before(&pos, &new_node);
    ++size_;
  }

  /// Remove a specific node from the list. O(1).
  /// Precondition: node must be linked and belong to THIS list.
  /// (Belonging is not checked — the caller must ensure correctness.)
  void erase(T &node) noexcept {
    assert(node.is_linked() && "erase: node is not linked");
    unlink(&node);
    --size_;
    assert(!node.is_linked() && "node must be unlinked after erase");
  }

  /// Unlink all nodes, resetting each node's prev/next to nullptr.
  /// O(n) — must visit each node to clear its link state.
  void clear() noexcept {
#ifndef NDEBUG
    std::size_t cleared = 0;
#endif
    IntrusiveListHook *cur = sentinel_.next;
    while (cur != &sentinel_) {
      IntrusiveListHook *next = cur->next;
      cur->prev = nullptr;
      cur->next = nullptr;
      cur = next;
#ifndef NDEBUG
      ++cleared;
#endif
    }
    assert(cleared == size_ && "cleared count must match size_");
    init_sentinel();
    size_ = 0;
  }

  // ---------------------------------------------------------------------------
  // Iteration (bidirectional)
  // ---------------------------------------------------------------------------

  [[nodiscard]] Iterator begin() noexcept { return Iterator{sentinel_.next}; }
  [[nodiscard]] Iterator end() noexcept { return Iterator{&sentinel_}; }

  [[nodiscard]] ConstIterator begin() const noexcept {
    return ConstIterator{sentinel_.next};
  }
  [[nodiscard]] ConstIterator end() const noexcept {
    return ConstIterator{&sentinel_};
  }

  [[nodiscard]] ConstIterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] ConstIterator cend() const noexcept { return end(); }

  // ===========================================================================
  // Iterator implementation
  // ===========================================================================
  //
  // Bidirectional iterator wrapping a single hook pointer (8 bytes).
  // operator* performs static_cast<T&> to recover the derived type.
  // end() iterator points to the sentinel — dereferencing end() is UB
  // (same contract as std::list::end). Debug assert catches this.
  //
  // Template parameter IsConst:
  //   false -> Iterator  (yields T&)
  //   true  -> ConstIterator (const yields T&)

  template <bool IsConst> class IteratorImpl {
    friend IntrusiveList;
    template <bool> friend class IteratorImpl;

    using HookPtr = std::conditional_t<IsConst, const IntrusiveListHook *,
                                       IntrusiveListHook *>;

    HookPtr hook_{nullptr};

    explicit IteratorImpl(HookPtr hook) noexcept : hook_(hook) {}

  public:
    // STL iterator traits.
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<IsConst, const T *, T *>;
    using reference = std::conditional_t<IsConst, const T &, T &>;

    IteratorImpl() noexcept = default;

    /// Implicit conversion: Iterator -> ConstIterator.
    template <bool OtherConst>
      requires(IsConst && !OtherConst)
    // NOLINTNEXTLINE(google-explicit-constructor)
    IteratorImpl(const IteratorImpl<OtherConst> &other) noexcept
        : hook_(other.hook_) {}

    /// Dereferencing end() is UB — the sentinel is not a T, so
    /// static_cast would be an invalid downcast. Same contract as
    /// std::list::end(). We intentionally omit a sentinel guard here
    /// to keep the iterator at 8 bytes (single pointer).
    reference operator*() const noexcept {
      return static_cast<reference>(*hook_);
    }

    pointer operator->() const noexcept { return static_cast<pointer>(hook_); }

    IteratorImpl &operator++() noexcept {
      hook_ = hook_->next;
      return *this;
    }

    IteratorImpl operator++(int) noexcept {
      auto tmp = *this;
      hook_ = hook_->next;
      return tmp;
    }

    IteratorImpl &operator--() noexcept {
      hook_ = hook_->prev;
      return *this;
    }

    IteratorImpl operator--(int) noexcept {
      auto tmp = *this;
      hook_ = hook_->prev;
      return tmp;
    }

    // operator!= is synthesized from operator== in C++20.
    // No operator<=> — bidirectional iterators have no ordering.
    friend bool operator==(IteratorImpl a, IteratorImpl b) noexcept {
      return a.hook_ == b.hook_;
    }
  };

private:
  /// Reset sentinel to self-referencing state (empty list).
  void init_sentinel() noexcept {
    sentinel_.prev = &sentinel_;
    sentinel_.next = &sentinel_;
  }

  /// After swap/move, edge nodes still point to the old sentinel.
  /// Fix the first and last node (or self-reference if empty).
  void fix_sentinel_links() noexcept {
    if (size_ == 0) {
      init_sentinel();
    } else {
      sentinel_.next->prev = &sentinel_;
      sentinel_.prev->next = &sentinel_;
    }
  }

  /// Insert `node` immediately after `pos` in the circular chain.
  ///
  /// Before: ... <-> pos <-> pos->next <-> ...
  /// After:  ... <-> pos <-> node <-> pos->next <-> ...
  static void link_after(IntrusiveListHook *pos,
                         IntrusiveListHook *node) noexcept {
    node->prev = pos;
    node->next = pos->next;
    pos->next->prev = node;
    pos->next = node;
  }

  /// Insert `node` immediately before `pos` in the circular chain.
  ///
  /// Before: ... <-> pos->prev <-> pos <-> ...
  /// After:  ... <-> pos->prev <-> node <-> pos <-> ...
  static void link_before(IntrusiveListHook *pos,
                          IntrusiveListHook *node) noexcept {
    node->next = pos;
    node->prev = pos->prev;
    pos->prev->next = node;
    pos->prev = node;
  }

  /// Remove `node` from its circular chain and reset its pointers to nullptr.
  static void unlink(IntrusiveListHook *node) noexcept {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
  }
};

} // namespace mk::ds
