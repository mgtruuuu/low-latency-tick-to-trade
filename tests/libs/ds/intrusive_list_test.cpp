/**
 * @file intrusive_list_test.cpp
 * @brief Tests for mk::ds::IntrusiveList — intrusive doubly-linked list.
 */

#include "ds/intrusive_list.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace {

using mk::ds::IntrusiveList;
using mk::ds::IntrusiveListHook;

/// Simple test node: inherits the hook and carries an int payload.
struct TestNode : IntrusiveListHook {
  int value{0};
  explicit TestNode(int v) : value(v) {}
};

// =============================================================================
// 1. EmptyOnConstruction
// =============================================================================

TEST(IntrusiveListTest, EmptyOnConstruction) {
  IntrusiveList<TestNode> const list;
  EXPECT_EQ(list.size(), 0U);
  EXPECT_TRUE(list.empty());
}

// =============================================================================
// 2. PushBackAndAccess
// =============================================================================

TEST(IntrusiveListTest, PushBackAndAccess) {
  // Nodes must outlive the list: ~IntrusiveList calls clear(), which
  // traverses nodes to unlink them. C++ destroys locals in reverse
  // declaration order, so declare nodes before the list.
  TestNode n1(42);
  IntrusiveList<TestNode> list;

  list.push_back(n1);

  EXPECT_EQ(list.size(), 1U);
  EXPECT_FALSE(list.empty());
  EXPECT_EQ(list.front().value, 42);
  EXPECT_EQ(list.back().value, 42);
  // front and back should refer to the same node.
  EXPECT_EQ(&list.front(), &n1);
  EXPECT_EQ(&list.back(), &n1);
}

// =============================================================================
// 3. PushFrontAndAccess
// =============================================================================

TEST(IntrusiveListTest, PushFrontAndAccess) {
  TestNode n1(99);
  IntrusiveList<TestNode> list;

  list.push_front(n1);

  EXPECT_EQ(list.size(), 1U);
  EXPECT_FALSE(list.empty());
  EXPECT_EQ(list.front().value, 99);
  EXPECT_EQ(list.back().value, 99);
  EXPECT_EQ(&list.front(), &n1);
}

// =============================================================================
// 4. MixedPushOrder
// =============================================================================

TEST(IntrusiveListTest, MixedPushOrder) {
  TestNode n1(1);
  TestNode n2(2);
  TestNode n3(3);
  TestNode n4(4);
  IntrusiveList<TestNode> list;

  list.push_back(n2);  // [2]
  list.push_front(n1); // [1, 2]
  list.push_back(n3);  // [1, 2, 3]
  list.push_front(n4); // [4, 1, 2, 3]

  EXPECT_EQ(list.size(), 4U);
  EXPECT_EQ(list.front().value, 4);
  EXPECT_EQ(list.back().value, 3);

  // Verify full order via iteration.
  std::vector<int> const expected = {4, 1, 2, 3};
  std::vector<int> actual;
  for (const auto &node : list) {
    actual.push_back(node.value);
  }
  EXPECT_EQ(actual, expected);
}

// =============================================================================
// 5. PopFront
// =============================================================================

TEST(IntrusiveListTest, PopFront) {
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3); // [10, 20, 30]

  TestNode const &popped = list.pop_front();
  EXPECT_EQ(popped.value, 10);
  EXPECT_EQ(&popped, &n1);
  EXPECT_FALSE(n1.is_linked());

  EXPECT_EQ(list.size(), 2U);
  EXPECT_EQ(list.front().value, 20);
  EXPECT_EQ(list.back().value, 30);
}

// =============================================================================
// 6. PopBack
// =============================================================================

TEST(IntrusiveListTest, PopBack) {
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3); // [10, 20, 30]

  TestNode const &popped = list.pop_back();
  EXPECT_EQ(popped.value, 30);
  EXPECT_EQ(&popped, &n3);
  EXPECT_FALSE(n3.is_linked());

  EXPECT_EQ(list.size(), 2U);
  EXPECT_EQ(list.front().value, 10);
  EXPECT_EQ(list.back().value, 20);
}

// =============================================================================
// 7. EraseMiddle
// =============================================================================

TEST(IntrusiveListTest, EraseMiddle) {
  TestNode n1(1);
  TestNode n2(2);
  TestNode n3(3);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3); // [1, 2, 3]

  list.erase(n2); // [1, 3]

  EXPECT_EQ(list.size(), 2U);
  EXPECT_FALSE(n2.is_linked());
  EXPECT_EQ(list.front().value, 1);
  EXPECT_EQ(list.back().value, 3);

  // Verify linkage: front->next should be back.
  std::vector<int> actual;
  for (const auto &node : list) {
    actual.push_back(node.value);
  }
  std::vector<int> const expected = {1, 3};
  EXPECT_EQ(actual, expected);
}

// =============================================================================
// 8. EraseOnlyElement
// =============================================================================

TEST(IntrusiveListTest, EraseOnlyElement) {
  TestNode n1(42);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  ASSERT_EQ(list.size(), 1U);

  list.erase(n1);

  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0U);
  EXPECT_FALSE(n1.is_linked());
}

// =============================================================================
// 9. IsLinked
// =============================================================================

TEST(IntrusiveListTest, IsLinked) {
  TestNode n1(1);
  IntrusiveList<TestNode> list;

  // Not linked initially.
  EXPECT_FALSE(n1.is_linked());

  // Linked after push.
  list.push_back(n1);
  EXPECT_TRUE(n1.is_linked());

  // Unlinked after erase.
  list.erase(n1);
  EXPECT_FALSE(n1.is_linked());

  // Can re-insert after erase.
  list.push_front(n1);
  EXPECT_TRUE(n1.is_linked());
  EXPECT_EQ(list.size(), 1U);
  EXPECT_EQ(list.front().value, 1);
}

// =============================================================================
// 10. ClearUnlinksAll
// =============================================================================

TEST(IntrusiveListTest, ClearUnlinksAll) {
  TestNode nodes[5] = {TestNode(0), TestNode(1), TestNode(2), TestNode(3),
                       TestNode(4)};
  IntrusiveList<TestNode> list;

  for (auto &n : nodes) {
    list.push_back(n);
  }
  ASSERT_EQ(list.size(), 5U);

  list.clear();

  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0U);

  // Every node should be unlinked.
  for (const auto &n : nodes) {
    EXPECT_FALSE(n.is_linked());
  }

  // List should be reusable after clear.
  list.push_back(nodes[0]);
  EXPECT_EQ(list.size(), 1U);
  EXPECT_EQ(list.front().value, 0);
}

// =============================================================================
// 11. ForwardIteration
// =============================================================================

TEST(IntrusiveListTest, ForwardIteration) {
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  TestNode n4(40);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3);
  list.push_back(n4); // [10, 20, 30, 40]

  std::vector<int> actual;
  for (auto it = list.begin(); it != list.end(); ++it) {
    actual.push_back(it->value);
  }

  std::vector<int> const expected = {10, 20, 30, 40};
  EXPECT_EQ(actual, expected);
}

// =============================================================================
// 12. ReverseIteration
// =============================================================================

TEST(IntrusiveListTest, ReverseIteration) {
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  TestNode n4(40);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3);
  list.push_back(n4); // [10, 20, 30, 40]

  // Reverse iterate: start from --end(), go backwards to begin().
  std::vector<int> actual;
  auto it = list.end();
  while (it != list.begin()) {
    --it;
    actual.push_back(it->value);
  }

  std::vector<int> const expected = {40, 30, 20, 10};
  EXPECT_EQ(actual, expected);
}

// =============================================================================
// 13. ConstAccess
// =============================================================================

TEST(IntrusiveListTest, ConstAccess) {
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2);
  list.push_back(n3);

  const auto &clist = list;

  EXPECT_EQ(clist.size(), 3U);
  EXPECT_FALSE(clist.empty());
  EXPECT_EQ(clist.front().value, 10);
  EXPECT_EQ(clist.back().value, 30);

  // Const iteration.
  std::vector<int> actual;
  for (const auto &node : clist) {
    actual.push_back(node.value);
  }
  std::vector<int> const expected = {10, 20, 30};
  EXPECT_EQ(actual, expected);

  // Iterator -> ConstIterator implicit conversion.
  const IntrusiveList<TestNode>::ConstIterator cit = list.begin();
  EXPECT_EQ(cit->value, 10);
}

// =============================================================================
// 14. InsertBefore — mid-list insertion for maintaining sorted order
// =============================================================================

TEST(IntrusiveListTest, InsertBeforeMidList) {
  TestNode n1(10);
  TestNode n2(30);
  TestNode n3(20); // insert between n1 and n2
  IntrusiveList<TestNode> list;

  list.push_back(n1);
  list.push_back(n2); // [10, 30]

  list.insert_before(n2, n3); // [10, 20, 30]

  EXPECT_EQ(list.size(), 3U);

  std::vector<int> actual;
  for (const auto &node : list) {
    actual.push_back(node.value);
  }
  std::vector<int> const expected = {10, 20, 30};
  EXPECT_EQ(actual, expected);
}

TEST(IntrusiveListTest, InsertBeforeFront) {
  TestNode n1(20);
  TestNode n2(10); // insert before front
  IntrusiveList<TestNode> list;

  list.push_back(n1); // [20]

  list.insert_before(n1, n2); // [10, 20]

  EXPECT_EQ(list.size(), 2U);
  EXPECT_EQ(list.front().value, 10);
  EXPECT_EQ(list.back().value, 20);
}

TEST(IntrusiveListTest, InsertBeforeMaintainsSortedOrder) {
  // Simulate the order book pattern: insert into a sorted descending list.
  TestNode n100(100);
  TestNode n90(90);
  TestNode n80(80);
  IntrusiveList<TestNode> list;

  list.push_back(n100);
  list.push_back(n80); // [100, 80] — descending

  // Insert 90 before 80 (first element with price < 90).
  list.insert_before(n80, n90); // [100, 90, 80]

  std::vector<int> actual;
  for (const auto &node : list) {
    actual.push_back(node.value);
  }
  std::vector<int> const expected = {100, 90, 80};
  EXPECT_EQ(actual, expected);
}

// =============================================================================
// 15. Death tests — assert() precondition violations (debug builds only)
// =============================================================================

#ifndef NDEBUG

TEST(IntrusiveListDeathTest, FrontOnEmptyAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        (void)list.front();
      },
      "");
}

TEST(IntrusiveListDeathTest, BackOnEmptyAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        (void)list.back();
      },
      "");
}

TEST(IntrusiveListDeathTest, ConstFrontOnEmptyAborts) {
  EXPECT_DEATH(
      {
        const IntrusiveList<TestNode> list;
        const auto &clist = list;
        (void)clist.front();
      },
      "");
}

TEST(IntrusiveListDeathTest, ConstBackOnEmptyAborts) {
  EXPECT_DEATH(
      {
        const IntrusiveList<TestNode> list;
        const auto &clist = list;
        (void)clist.back();
      },
      "");
}

TEST(IntrusiveListDeathTest, PopFrontOnEmptyAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        (void)list.pop_front();
      },
      "");
}

TEST(IntrusiveListDeathTest, PopBackOnEmptyAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        (void)list.pop_back();
      },
      "");
}

TEST(IntrusiveListDeathTest, PushFrontLinkedNodeAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        TestNode n(1);
        list.push_back(n);
        list.push_front(n); // n is already linked
      },
      "");
}

TEST(IntrusiveListDeathTest, PushBackLinkedNodeAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        TestNode n(1);
        list.push_front(n);
        list.push_back(n); // n is already linked
      },
      "");
}

TEST(IntrusiveListDeathTest, InsertBeforeUnlinkedPosAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        TestNode pos(1);
        TestNode new_node(2);
        list.insert_before(pos, new_node); // pos is not linked
      },
      "");
}

TEST(IntrusiveListDeathTest, InsertBeforeLinkedNewNodeAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        TestNode pos(1);
        TestNode new_node(2);
        list.push_back(pos);
        list.push_back(new_node);
        list.insert_before(pos, new_node); // new_node is already linked
      },
      "");
}

TEST(IntrusiveListDeathTest, EraseUnlinkedNodeAborts) {
  EXPECT_DEATH(
      {
        IntrusiveList<TestNode> list;
        TestNode n(1);
        list.erase(n); // n is not linked
      },
      "");
}

#endif // NDEBUG

// =============================================================================
// Move semantics
// =============================================================================

TEST(IntrusiveListTest, MoveConstructEmpty) {
  IntrusiveList<TestNode> a;
  const IntrusiveList<TestNode> b(std::move(a));

  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0U);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(IntrusiveListTest, MoveConstructNonEmpty) {
  IntrusiveList<TestNode> a;
  TestNode n1(10);
  TestNode n2(20);
  TestNode n3(30);
  a.push_back(n1);
  a.push_back(n2);
  a.push_back(n3);

  IntrusiveList<TestNode> b(std::move(a));

  // b has all 3 nodes.
  EXPECT_EQ(b.size(), 3U);
  EXPECT_EQ(b.front().value, 10);
  EXPECT_EQ(b.back().value, 30);

  // a is empty.
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(a.size(), 0U);

  // Iteration works (sentinel links are correct).
  std::vector<int> values;
  for (const auto &node : b) {
    values.push_back(node.value);
  }
  EXPECT_EQ(values, (std::vector<int>{10, 20, 30}));
}

TEST(IntrusiveListTest, MoveAssignNonEmptyToEmpty) {
  IntrusiveList<TestNode> a;
  TestNode n1(1);
  TestNode n2(2);
  a.push_back(n1);
  a.push_back(n2);

  IntrusiveList<TestNode> b;
  b = std::move(a);

  EXPECT_EQ(b.size(), 2U);
  EXPECT_EQ(b.front().value, 1);
  EXPECT_EQ(b.back().value, 2);
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(IntrusiveListTest, MoveAssignNonEmptyToNonEmpty) {
  IntrusiveList<TestNode> a;
  TestNode a1(10);
  TestNode a2(20);
  a.push_back(a1);
  a.push_back(a2);

  IntrusiveList<TestNode> b;
  TestNode b1(99);
  b.push_back(b1);

  b = std::move(a);

  // b now has a's nodes.
  EXPECT_EQ(b.size(), 2U);
  EXPECT_EQ(b.front().value, 10);

  // a is empty.
  EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)

  // b1 was unlinked by clear() in move assignment.
  EXPECT_FALSE(b1.is_linked());
}

TEST(IntrusiveListTest, MoveConstructThenModify) {
  // Nodes must outlive the list — declare nodes before lists so they are
  // destroyed after lists (C++ destroys locals in reverse declaration order).
  TestNode n1(1);
  TestNode n2(2);
  TestNode n3(3);
  IntrusiveList<TestNode> a;
  a.push_back(n1);
  a.push_back(n2);

  IntrusiveList<TestNode> b(std::move(a));

  // Modify b — push/pop/erase must work after move.
  b.push_back(n3);
  EXPECT_EQ(b.size(), 3U);
  EXPECT_EQ(b.back().value, 3);

  b.erase(n2);
  EXPECT_EQ(b.size(), 2U);

  auto &popped = b.pop_front();
  EXPECT_EQ(popped.value, 1);
  EXPECT_EQ(b.size(), 1U);
  EXPECT_EQ(b.front().value, 3);
}

// =============================================================================
// Swap
// =============================================================================

TEST(IntrusiveListTest, SwapBothNonEmpty) {
  // Nodes must outlive lists — declare nodes first.
  TestNode a1(10);
  TestNode a2(20);
  TestNode b1(77);
  IntrusiveList<TestNode> a;
  a.push_back(a1);
  a.push_back(a2);

  IntrusiveList<TestNode> b;
  b.push_back(b1);

  a.swap(b);

  // a now has b's node.
  EXPECT_EQ(a.size(), 1U);
  EXPECT_EQ(a.front().value, 77);

  // b now has a's nodes.
  EXPECT_EQ(b.size(), 2U);
  EXPECT_EQ(b.front().value, 10);
  EXPECT_EQ(b.back().value, 20);
}

TEST(IntrusiveListTest, SwapOneEmptyOneNonEmpty) {
  TestNode n1(42);
  IntrusiveList<TestNode> a;
  a.push_back(n1);

  IntrusiveList<TestNode> b; // empty

  swap(a, b); // ADL friend swap

  EXPECT_TRUE(a.empty());
  EXPECT_EQ(b.size(), 1U);
  EXPECT_EQ(b.front().value, 42);
}

TEST(IntrusiveListTest, SwapBothEmpty) {
  IntrusiveList<TestNode> a;
  IntrusiveList<TestNode> b;

  a.swap(b);

  EXPECT_TRUE(a.empty());
  EXPECT_TRUE(b.empty());
}

} // namespace
