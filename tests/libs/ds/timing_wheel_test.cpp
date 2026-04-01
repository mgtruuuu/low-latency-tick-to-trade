/**
 * @file timing_wheel_test.cpp
 * @brief Tests for TimingWheel — runtime-capacity timing wheel.
 */

#include "ds/timing_wheel.hpp"
#include "sys/memory/aligned_array_deleter.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

namespace {

// =============================================================================
// Test fixture
// =============================================================================

class TimingWheelRtTest : public ::testing::Test {
protected:
  static constexpr std::size_t kWheelSize = 16;
  static constexpr std::size_t kMaxTimers = 32;

  using Wheel = mk::ds::TimingWheel;
  using handle_t = Wheel::handle_t;
  using tick_t = Wheel::tick_t;
  using cb_t = Wheel::cb_t;

  // Allocate a properly aligned buffer for the timing wheel.
  std::size_t buf_size_ = Wheel::required_buffer_size(kWheelSize, kMaxTimers);
  mk::sys::memory::AlignedByteArray<> buf_{
      new (std::align_val_t{alignof(std::max_align_t)}) std::byte[buf_size_]};

  // Create the wheel using the factory.
  Wheel wheel_{[this]() {
    auto opt = Wheel::create(buf_.get(), buf_size_, kWheelSize, kMaxTimers);
    if (!opt) {
      std::abort();
    }
    return std::move(*opt); // NOLINT(bugprone-unchecked-optional-access)
  }()};

  /// Simple counter callback: increments an int pointed to by ctx.
  static void increment_cb(void *ctx) noexcept { ++(*static_cast<int *>(ctx)); }

  /// Callback that records firing order.
  struct RecordCtx {
    std::vector<int> *log;
    int id;
  };

  static void record_cb(void *ctx) noexcept {
    auto *rc = static_cast<RecordCtx *>(ctx);
    rc->log->push_back(rc->id);
  }
};

// =============================================================================
// Construction — create() factory
// =============================================================================

TEST_F(TimingWheelRtTest, CreateFactorySuccess) {
  auto buf = std::make_unique<std::byte[]>(
      Wheel::required_buffer_size(kWheelSize, kMaxTimers) + 64);
  // Align the buffer.
  void *aligned = buf.get();
  std::size_t space = Wheel::required_buffer_size(kWheelSize, kMaxTimers) + 64;
  std::align(alignof(std::max_align_t), 1, aligned, space);

  auto opt = Wheel::create(aligned,
                           Wheel::required_buffer_size(kWheelSize, kMaxTimers),
                           kWheelSize, kMaxTimers);
  ASSERT_TRUE(opt.has_value());
  EXPECT_EQ(kWheelSize,
            opt->wheel_size()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(kMaxTimers,
            opt->max_timers()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U,
            opt->active_count()); // NOLINT(bugprone-unchecked-optional-access)
  EXPECT_EQ(0U,
            opt->current_tick()); // NOLINT(bugprone-unchecked-optional-access)
}

TEST_F(TimingWheelRtTest, CreateWithNullptrReturnsFalse) {
  auto opt = Wheel::create(nullptr, 1024, kWheelSize, kMaxTimers);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, CreateWithUndersizedBufferReturnsFalse) {
  auto buf = std::make_unique<std::byte[]>(16);
  auto opt = Wheel::create(buf.get(), 16, kWheelSize, kMaxTimers);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, CreateWithNonPow2WheelSizeReturnsFalse) {
  auto big_buf = std::make_unique<std::byte[]>(1 << 20);
  void *aligned = big_buf.get();
  std::size_t space = 1 << 20;
  std::align(alignof(std::max_align_t), 1, aligned, space);

  auto opt = Wheel::create(aligned, space, 13, kMaxTimers); // 13 not pow2
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, CreateWithNonPow2MaxTimersReturnsFalse) {
  auto big_buf = std::make_unique<std::byte[]>(1 << 20);
  void *aligned = big_buf.get();
  std::size_t space = 1 << 20;
  std::align(alignof(std::max_align_t), 1, aligned, space);

  auto opt = Wheel::create(aligned, space, kWheelSize, 17); // 17 not pow2
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, CreateWithWheelSizeOneReturnsFalse) {
  auto big_buf = std::make_unique<std::byte[]>(1 << 20);
  void *aligned = big_buf.get();
  std::size_t space = 1 << 20;
  std::align(alignof(std::max_align_t), 1, aligned, space);

  auto opt = Wheel::create(aligned, space, 1, kMaxTimers);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, CreateWithMisalignedBufferReturnsFalse) {
  // Create a buffer and offset by 1 byte to misalign.
  auto big_buf = std::make_unique<std::byte[]>(
      Wheel::required_buffer_size(kWheelSize, kMaxTimers) + 64);
  void *misaligned = big_buf.get() + 1;

  auto opt = Wheel::create(misaligned,
                           Wheel::required_buffer_size(kWheelSize, kMaxTimers),
                           kWheelSize, kMaxTimers);
  EXPECT_FALSE(opt.has_value());
}

TEST_F(TimingWheelRtTest, DefaultConstructorCreatesUnusableWheel) {
  const Wheel w;
  EXPECT_EQ(0U, w.wheel_size());
  EXPECT_EQ(0U, w.max_timers());
  EXPECT_EQ(0U, w.active_count());
}

// =============================================================================
// Static helpers
// =============================================================================

TEST_F(TimingWheelRtTest, RequiredBufferSizeCalculation) {
  // Exact value depends on TimerNode size and alignment padding.
  // At minimum: ws * 4 + mt * sizeof(TimerNode) + mt * 4.
  auto sz = Wheel::required_buffer_size(16, 32);
  EXPECT_GT(sz, 0U);

  // Doubling max_timers should roughly double the node+free portion.
  auto sz2 = Wheel::required_buffer_size(16, 64);
  EXPECT_GT(sz2, sz);
}

TEST_F(TimingWheelRtTest, RoundUpCapacity) {
  EXPECT_EQ(0U, Wheel::round_up_capacity(0));
  EXPECT_EQ(2U, Wheel::round_up_capacity(1));
  EXPECT_EQ(2U, Wheel::round_up_capacity(2));
  EXPECT_EQ(4U, Wheel::round_up_capacity(3));
  EXPECT_EQ(4U, Wheel::round_up_capacity(4));
  EXPECT_EQ(8U, Wheel::round_up_capacity(5));
  EXPECT_EQ(16U, Wheel::round_up_capacity(16));
  EXPECT_EQ(256U, Wheel::round_up_capacity(200));
}

// =============================================================================
// Basic schedule + tick
// =============================================================================

TEST_F(TimingWheelRtTest, ScheduleAndTickFiresCallback) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 1);
  ASSERT_NE(Wheel::kInvalidHandle, h);
  EXPECT_EQ(1U, wheel_.active_count());

  auto fired = wheel_.tick();
  EXPECT_EQ(1U, fired);
  EXPECT_EQ(1, counter);
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelRtTest, ScheduleWithDelay) {
  int counter = 0;
  (void)wheel_.schedule(increment_cb, &counter, 5);

  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);

  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, counter);
}

// =============================================================================
// Multiple timers in same bucket
// =============================================================================

TEST_F(TimingWheelRtTest, MultipleTimersSameBucket) {
  int c1 = 0;
  int c2 = 0;
  int c3 = 0;
  (void)wheel_.schedule(increment_cb, &c1, 1);
  (void)wheel_.schedule(increment_cb, &c2, 1);
  (void)wheel_.schedule(increment_cb, &c3, 1);

  EXPECT_EQ(3U, wheel_.active_count());

  auto fired = wheel_.tick();
  EXPECT_EQ(3U, fired);
  EXPECT_EQ(1, c1);
  EXPECT_EQ(1, c2);
  EXPECT_EQ(1, c3);
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Cancel
// =============================================================================

TEST_F(TimingWheelRtTest, CancelPreventsCallback) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 3);

  EXPECT_TRUE(wheel_.cancel(h));
  EXPECT_EQ(0U, wheel_.active_count());

  for (int i = 0; i < 5; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(0, counter);
}

TEST_F(TimingWheelRtTest, CancelInvalidHandleReturnsFalse) {
  EXPECT_FALSE(wheel_.cancel(Wheel::kInvalidHandle));
}

TEST_F(TimingWheelRtTest, CancelStaleHandleReturnsFalse) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 1);

  wheel_.tick();
  EXPECT_EQ(1, counter);

  EXPECT_FALSE(wheel_.cancel(h));
}

TEST_F(TimingWheelRtTest, DoubleCancelReturnsFalse) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 5);

  EXPECT_TRUE(wheel_.cancel(h));
  EXPECT_FALSE(wheel_.cancel(h));
}

TEST_F(TimingWheelRtTest, CancelMiddleOfBucket) {
  int c1 = 0;
  int c2 = 0;
  int c3 = 0;
  (void)wheel_.schedule(increment_cb, &c1, 2);
  auto h2 = wheel_.schedule(increment_cb, &c2, 2);
  (void)wheel_.schedule(increment_cb, &c3, 2);

  EXPECT_TRUE(wheel_.cancel(h2));
  EXPECT_EQ(2U, wheel_.active_count());

  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(1, c1);
  EXPECT_EQ(0, c2);
  EXPECT_EQ(1, c3);
}

// =============================================================================
// Pool exhaustion
// =============================================================================

TEST_F(TimingWheelRtTest, PoolExhaustionReturnsInvalidHandle) {
  int counter = 0;

  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter, 1);
    ASSERT_NE(Wheel::kInvalidHandle, h) << "exhausted at i=" << i;
  }
  EXPECT_EQ(kMaxTimers, wheel_.active_count());

  auto h = wheel_.schedule(increment_cb, &counter, 1);
  EXPECT_EQ(Wheel::kInvalidHandle, h);
}

TEST_F(TimingWheelRtTest, PoolExhaustionRecoveryAfterFiring) {
  int counter = 0;

  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, 1);
  }

  wheel_.tick();
  EXPECT_EQ(static_cast<int>(kMaxTimers), counter);
  EXPECT_EQ(0U, wheel_.active_count());

  auto h = wheel_.schedule(increment_cb, &counter, 1);
  EXPECT_NE(Wheel::kInvalidHandle, h);
}

// =============================================================================
// Empty bucket tick
// =============================================================================

TEST_F(TimingWheelRtTest, TickWithEmptyBucketReturnsZero) {
  EXPECT_EQ(0U, wheel_.tick());
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Full rotation (wrap-around)
// =============================================================================

TEST_F(TimingWheelRtTest, WrapAroundFullRotation) {
  int counter = 0;

  (void)wheel_.schedule(increment_cb, &counter, kWheelSize - 1);

  for (std::size_t i = 0; i < kWheelSize - 2; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);

  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, counter);
}

TEST_F(TimingWheelRtTest, TimerAfterManyRotations) {
  for (std::size_t i = 0; i < kWheelSize * 3; ++i) {
    wheel_.tick();
  }

  int counter = 0;
  (void)wheel_.schedule(increment_cb, &counter, 5);

  for (int i = 0; i < 4; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(0, counter);

  wheel_.tick();
  EXPECT_EQ(1, counter);
}

// =============================================================================
// Reset
// =============================================================================

TEST_F(TimingWheelRtTest, ResetClearsAllTimers) {
  int counter = 0;
  for (int i = 1; i < 10; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, (i % (kWheelSize - 1)) + 1);
  }
  EXPECT_EQ(9U, wheel_.active_count());

  wheel_.reset();
  EXPECT_EQ(0U, wheel_.active_count());
  EXPECT_EQ(0U, wheel_.current_tick());

  for (std::size_t i = 0; i < kWheelSize; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);
}

TEST_F(TimingWheelRtTest, ResetFreesPoolForReuse) {
  int counter = 0;

  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, 1);
  }
  EXPECT_EQ(Wheel::kInvalidHandle, wheel_.schedule(increment_cb, &counter, 1));

  wheel_.reset();
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter, 1);
    EXPECT_NE(Wheel::kInvalidHandle, h) << "failed at i=" << i;
  }
}

// =============================================================================
// Observers
// =============================================================================

TEST_F(TimingWheelRtTest, CurrentTickAdvances) {
  EXPECT_EQ(0U, wheel_.current_tick());
  wheel_.tick();
  EXPECT_EQ(1U, wheel_.current_tick());
  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(3U, wheel_.current_tick());
}

TEST_F(TimingWheelRtTest, RuntimeCapacityObservers) {
  EXPECT_EQ(kWheelSize, wheel_.wheel_size());
  EXPECT_EQ(kMaxTimers, wheel_.max_timers());
}

// =============================================================================
// Reentrancy: callback schedules new timer to same slot
// =============================================================================

TEST_F(TimingWheelRtTest, CallbackSchedulesToSameSlot) {
  struct Ctx {
    Wheel *wheel;
    int fire_count;
    cb_t self_cb;
  };

  const static auto reschedule_cb = [](void *raw) noexcept {
    auto *c = static_cast<Ctx *>(raw);
    ++c->fire_count;
    if (c->fire_count < 3) {
      (void)c->wheel->schedule(c->self_cb, c, c->wheel->wheel_size() - 1);
    }
  };

  static void (*const wrapper)(void *) noexcept = +reschedule_cb;

  Ctx ctx{.wheel = &wheel_, .fire_count = 0, .self_cb = wrapper};
  (void)wheel_.schedule(wrapper, &ctx, 1);

  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, ctx.fire_count);
  EXPECT_EQ(1U, wheel_.active_count());

  for (std::size_t i = 0; i < wheel_.wheel_size() - 2; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }

  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(2, ctx.fire_count);
}

// =============================================================================
// Reentrancy: callback cancels another timer
// =============================================================================

TEST_F(TimingWheelRtTest, CallbackCancelsOtherTimerAlreadyDetached) {
  handle_t other_handle = Wheel::kInvalidHandle;

  struct CancelCtx {
    Wheel *wheel;
    handle_t *target;
    int fired;
  };

  const static auto cancel_other_cb = [](void *raw) noexcept {
    auto *c = static_cast<CancelCtx *>(raw);
    ++c->fired;
    c->wheel->cancel(*c->target);
  };

  static void (*const wrapper)(void *) noexcept = +cancel_other_cb;

  int dummy = 0;
  other_handle = wheel_.schedule(increment_cb, &dummy, 5);

  CancelCtx ctx{.wheel = &wheel_, .target = &other_handle, .fired = 0};
  (void)wheel_.schedule(wrapper, &ctx, 1);

  wheel_.tick();
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(0U, wheel_.active_count());

  for (int i = 0; i < 10; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(0, dummy);
}

// =============================================================================
// Reentrancy: callback cancels a LATER node in the SAME detached chain.
// =============================================================================

TEST_F(TimingWheelRtTest, CallbackCancelsLaterNodeInSameChain) {
  int a_fired = 0;
  int b_fired = 0;

  const handle_t a_handle = wheel_.schedule(increment_cb, &a_fired, 1);
  (void)wheel_.schedule(increment_cb, &b_fired, 1);

  struct CancelCtx {
    Wheel *wheel;
    handle_t target;
    int fired;
  };
  CancelCtx ctx{.wheel = &wheel_, .target = a_handle, .fired = 0};

  const static auto cancel_later_cb = [](void *raw) noexcept {
    auto *c = static_cast<CancelCtx *>(raw);
    ++c->fired;
    c->wheel->cancel(c->target);
  };
  static void (*const wrapper)(void *) noexcept = +cancel_later_cb;

  (void)wheel_.schedule(wrapper, &ctx, 1);

  EXPECT_EQ(3U, wheel_.active_count());

  const auto fired = wheel_.tick();
  EXPECT_EQ(2U, fired);
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(1, b_fired);
  EXPECT_EQ(0, a_fired);
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelRtTest, CallbackCancelsMultipleLaterNodesInSameChain) {
  int a_fired = 0;
  int b_fired = 0;
  int c_fired = 0;

  const handle_t a_handle = wheel_.schedule(increment_cb, &a_fired, 1);
  const handle_t b_handle = wheel_.schedule(increment_cb, &b_fired, 1);
  (void)wheel_.schedule(increment_cb, &c_fired, 1);

  struct MultiCancelCtx {
    Wheel *wheel;
    handle_t target_a;
    handle_t target_b;
    int fired;
  };
  MultiCancelCtx ctx{
      .wheel = &wheel_, .target_a = a_handle, .target_b = b_handle, .fired = 0};

  const static auto cancel_multi_cb = [](void *raw) noexcept {
    auto *c = static_cast<MultiCancelCtx *>(raw);
    ++c->fired;
    c->wheel->cancel(c->target_a);
    c->wheel->cancel(c->target_b);
  };
  static void (*const wrapper)(void *) noexcept = +cancel_multi_cb;

  (void)wheel_.schedule(wrapper, &ctx, 1);

  EXPECT_EQ(4U, wheel_.active_count());

  const auto fired = wheel_.tick();
  EXPECT_EQ(2U, fired);
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(1, c_fired);
  EXPECT_EQ(0, b_fired);
  EXPECT_EQ(0, a_fired);
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Self-cancel during callback
// =============================================================================

TEST_F(TimingWheelRtTest, SelfCancelDuringCallback) {
  struct SelfCancelCtx {
    Wheel *wheel;
    handle_t my_handle;
    bool cancel_result;
    int fired;
  };

  const static auto self_cancel_cb = [](void *raw) noexcept {
    auto *c = static_cast<SelfCancelCtx *>(raw);
    ++c->fired;
    c->cancel_result = c->wheel->cancel(c->my_handle);
  };

  static void (*const wrapper)(void *) noexcept = +self_cancel_cb;

  SelfCancelCtx ctx{.wheel = &wheel_,
                    .my_handle = Wheel::kInvalidHandle,
                    .cancel_result = false,
                    .fired = 0};
  ctx.my_handle = wheel_.schedule(wrapper, &ctx, 1);
  ASSERT_NE(Wheel::kInvalidHandle, ctx.my_handle);

  wheel_.tick();
  EXPECT_EQ(1, ctx.fired);
  EXPECT_FALSE(ctx.cancel_result);
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelRtTest, SelfCancelDoesNotCorruptOtherTimers) {
  struct Ctx {
    Wheel *wheel;
    handle_t my_handle;
    int fired;
  };

  const static auto self_cancel_and_schedule_cb = [](void *raw) noexcept {
    auto *c = static_cast<Ctx *>(raw);
    ++c->fired;
    c->wheel->cancel(c->my_handle);
  };

  static void (*const wrapper)(void *) noexcept = +self_cancel_and_schedule_cb;

  int other_counter = 0;
  Ctx ctx{.wheel = &wheel_, .my_handle = Wheel::kInvalidHandle, .fired = 0};
  ctx.my_handle = wheel_.schedule(wrapper, &ctx, 1);

  (void)wheel_.schedule(increment_cb, &other_counter, 3);

  wheel_.tick();
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(1U, wheel_.active_count());

  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(1, other_counter);
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Generation counter: reused node slot, old handle is stale
// =============================================================================

TEST_F(TimingWheelRtTest, GenerationCounterPreventsABA) {
  int c1 = 0;
  int c2 = 0;

  auto h1 = wheel_.schedule(increment_cb, &c1, 1);
  wheel_.tick();
  EXPECT_EQ(1, c1);

  auto h2 = wheel_.schedule(increment_cb, &c2, 1);

  EXPECT_FALSE(wheel_.cancel(h1));
  EXPECT_TRUE(wheel_.cancel(h2));
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Move semantics
// =============================================================================

TEST_F(TimingWheelRtTest, MoveConstruction) {
  int counter = 0;
  (void)wheel_.schedule(increment_cb, &counter, 1);

  Wheel moved(std::move(wheel_));
  EXPECT_EQ(kWheelSize, moved.wheel_size());
  EXPECT_EQ(kMaxTimers, moved.max_timers());
  EXPECT_EQ(1U, moved.active_count());

  // Source is in default state.
  EXPECT_EQ(0U, wheel_.wheel_size());
  EXPECT_EQ(0U, wheel_.max_timers());

  // The moved-to wheel still works.
  moved.tick();
  EXPECT_EQ(1, counter);
  EXPECT_EQ(0U, moved.active_count());
}

TEST_F(TimingWheelRtTest, MoveAssignment) {
  int counter = 0;
  (void)wheel_.schedule(increment_cb, &counter, 2);

  Wheel other;
  other = std::move(wheel_);
  EXPECT_EQ(kWheelSize, other.wheel_size());
  EXPECT_EQ(1U, other.active_count());

  // Source is in default state.
  EXPECT_EQ(0U, wheel_.wheel_size());

  other.tick();
  other.tick();
  EXPECT_EQ(1, counter);
}

// =============================================================================
// Stress: fill and drain
// =============================================================================

TEST_F(TimingWheelRtTest, FillDrainCycles) {
  int counter = 0;

  for (int cycle = 0; cycle < 10; ++cycle) {
    for (std::size_t i = 0; i < kMaxTimers; ++i) {
      const tick_t delay = (i % (kWheelSize - 1)) + 1;
      auto h = wheel_.schedule(increment_cb, &counter, delay);
      ASSERT_NE(Wheel::kInvalidHandle, h) << "cycle=" << cycle << " i=" << i;
    }

    for (std::size_t i = 0; i < kWheelSize; ++i) {
      wheel_.tick();
    }

    EXPECT_EQ(0U, wheel_.active_count())
        << "cycle=" << cycle << " has leftover timers";
  }

  EXPECT_EQ(static_cast<int>(kMaxTimers * 10), counter);
}

// =============================================================================
// Mixed schedule and cancel
// =============================================================================

TEST_F(TimingWheelRtTest, MixedScheduleCancel) {
  int counter = 0;
  std::vector<handle_t> handles;

  for (int i = 0; i < 20; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter,
                             static_cast<tick_t>(i % (kWheelSize - 1)) + 1);
    ASSERT_NE(Wheel::kInvalidHandle, h);
    handles.push_back(h);
  }

  for (std::size_t i = 0; i < handles.size(); i += 2) {
    EXPECT_TRUE(wheel_.cancel(handles[i]));
  }
  EXPECT_EQ(10U, wheel_.active_count());

  for (std::size_t i = 0; i < kWheelSize; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(10, counter);
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Death tests (debug only)
// =============================================================================

#ifndef NDEBUG

using TimingWheelRtDeathTest = TimingWheelRtTest;

TEST_F(TimingWheelRtDeathTest, ScheduleWithDelayZeroAborts) {
  int counter = 0;
  EXPECT_DEATH({ (void)wheel_.schedule(increment_cb, &counter, 0); }, "");
}

TEST_F(TimingWheelRtDeathTest, ScheduleWithDelayTooLargeAborts) {
  int counter = 0;
  EXPECT_DEATH(
      { (void)wheel_.schedule(increment_cb, &counter, kWheelSize); }, "");
}

TEST_F(TimingWheelRtDeathTest, ScheduleWithNullCallbackAborts) {
  int counter = 0;
  EXPECT_DEATH({ (void)wheel_.schedule(nullptr, &counter, 1); }, "");
}

TEST_F(TimingWheelRtDeathTest, DirectConstructorAbortsOnInvalidInput) {
  EXPECT_DEATH(
      {
        // wheel_size = 13 (not power of 2) → abort.
        auto buf = std::make_unique<std::byte[]>(1 << 20);
        const Wheel w(buf.get(), 1 << 20, 13, 32);
        (void)w;
      },
      "");
}

#endif // NDEBUG

} // namespace
