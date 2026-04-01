/**
 * @file shared_exchange_queue_test.cpp
 * @brief Tests for SharedExchangeRegion — app-level shared memory layout.
 *
 * Generic FixedSharedSPSCQueue logic is tested in
 * tests/libs/sys/memory/fixed_shared_spsc_queue_test.cpp.
 * This file tests app-specific types: SharedExchangeRegion control flags,
 * queue-inside-region access, and placement new with the full region layout.
 */

#include "simulated_exchange/shared_exchange_queue.hpp"

#include "algo/trading_types.hpp"
#include "sys/memory/mmap_region.hpp"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace {

using mk::algo::Price;
using mk::app::EventType;
using mk::app::ExchangeEvent;
using mk::app::SharedExchangeRegion;

// ---------------------------------------------------------------------------
// Helper: create an event with identifiable fields
// ---------------------------------------------------------------------------

ExchangeEvent make_event(EventType type, std::uint32_t symbol_id,
                         Price price) noexcept {
  return ExchangeEvent{
      .type = type,
      .symbol_id = symbol_id,
      .price = price,
  };
}

// ---------------------------------------------------------------------------
// SharedExchangeRegion layout
// ---------------------------------------------------------------------------

TEST(SharedExchangeRegionTest, ControlFlagsDefaultToZero) {
  const SharedExchangeRegion region{};
  EXPECT_EQ(region.engine_ready.load(), 0U);
  EXPECT_EQ(region.shutdown.load(), 0U);
}

TEST(SharedExchangeRegionTest, QueueWorksInsideRegion) {
  SharedExchangeRegion region{};
  auto &q = region.md_event_queue;

  const auto event = make_event(EventType::kCancelAck, 2, 50'000);
  ASSERT_TRUE(q.try_push(event));

  ExchangeEvent out{};
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(out.type, EventType::kCancelAck);
  EXPECT_EQ(out.symbol_id, 2U);
}

TEST(SharedExchangeRegionTest, PerGatewayQueuesAreIsolated) {
  SharedExchangeRegion region{};

  // Push to gateway 2's request queue.
  const mk::app::OrderRequest req{
      .type = mk::app::OrderRequestType::kNewOrder,
      .gateway_id = 2,
      .symbol_id = 1,
      .price = 99'000,
  };
  ASSERT_TRUE(region.request_queues[2].try_push(req));

  // Gateway 0's queue should be empty.
  mk::app::OrderRequest out{};
  EXPECT_FALSE(region.request_queues[0].try_pop(out));

  // Gateway 2's queue has the request.
  ASSERT_TRUE(region.request_queues[2].try_pop(out));
  EXPECT_EQ(out.gateway_id, 2);
  EXPECT_EQ(out.price, 99'000);
}

TEST(SharedExchangeRegionTest, GatewayIdPreservedInOrderRequest) {
  SharedExchangeRegion region{};

  const mk::app::OrderRequest req{
      .type = mk::app::OrderRequestType::kClientConnect,
      .gateway_id = 5,
      .request_seq = 42,
  };
  ASSERT_TRUE(region.request_queues[5].try_push(req));

  mk::app::OrderRequest out{};
  ASSERT_TRUE(region.request_queues[5].try_pop(out));
  EXPECT_EQ(out.gateway_id, 5);
  EXPECT_EQ(out.request_seq, 42U);
}

// ---------------------------------------------------------------------------
// Placement new in real POSIX shared memory
// ---------------------------------------------------------------------------

TEST(SharedExchangeRegionTest, PlacementNewInSharedMemory) {
  // This test exercises the exact path used by exchange_engine:
  //   shm_open → mmap → placement new → push/pop → munmap → shm_unlink

  static constexpr std::string_view kTestShmName = "/mk_test_exchange_queue";
  const auto size = sizeof(SharedExchangeRegion);

  // Producer side: create shared memory.
  auto producer_region = mk::sys::memory::MmapRegion::open_shared(
      kTestShmName, size, mk::sys::memory::ShmMode::kCreateOrOpen,
      mk::sys::memory::PrefaultPolicy::kPopulateWrite);
  ASSERT_TRUE(producer_region.has_value())
      << "Failed to create shared memory (may need permission)";

  // Placement new — construct the region in shared memory.
  // NOLINTNEXTLINE(*-owning-memory, bugprone-unchecked-optional-access)
  auto *shared = new (producer_region->data()) SharedExchangeRegion{};

  // Signal readiness (acquire/release pair with consumer).
  shared->engine_ready.store(1U, std::memory_order_release);

  // Consumer side: open existing shared memory.
  auto consumer_region = mk::sys::memory::MmapRegion::open_shared(
      kTestShmName, size, mk::sys::memory::ShmMode::kOpenExisting,
      mk::sys::memory::PrefaultPolicy::kPopulateRead);
  ASSERT_TRUE(consumer_region.has_value());

  // Consumer maps to the same physical pages — cast, don't construct.
  // reinterpret_cast is required here: MmapRegion::data() returns
  // std::byte*, and static_cast cannot convert std::byte* to an
  // unrelated type. This is safe because we know the shared memory
  // was constructed via placement new with the correct layout.
  auto *consumer_raw =
      consumer_region->data(); // NOLINT(bugprone-unchecked-optional-access)
  // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
  auto *consumer_shared =
      reinterpret_cast<SharedExchangeRegion *>(consumer_raw);

  // Verify the acquire/release synchronization of engine_ready.
  EXPECT_EQ(consumer_shared->engine_ready.load(std::memory_order_acquire), 1U);

  // Producer pushes an event.
  const auto event = make_event(EventType::kFill, 1, 99'500);
  ASSERT_TRUE(shared->md_event_queue.try_push(event));

  // Consumer pops it — through a different virtual mapping of the same
  // physical memory. This is the core cross-process mechanism.
  ExchangeEvent out{};
  ASSERT_TRUE(consumer_shared->md_event_queue.try_pop(out));
  EXPECT_EQ(out.type, EventType::kFill);
  EXPECT_EQ(out.symbol_id, 1U);
  EXPECT_EQ(out.price, 99'500);

  // Cleanup: unlink the shared memory name.
  // The mappings survive until munmap (MmapRegion destructor).
  ::shm_unlink(std::string(kTestShmName).c_str());
}

} // namespace
