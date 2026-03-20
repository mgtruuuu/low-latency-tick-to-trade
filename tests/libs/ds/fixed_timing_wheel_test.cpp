/**
 * @file fixed_timing_wheel_test.cpp
 * @brief Tests for FixedTimingWheel — fixed-capacity timing wheel.
 */

#include "ds/fixed_timing_wheel.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

// =============================================================================
// Test fixture
// =============================================================================

/// Small wheel for unit tests: 16 slots, up to 32 concurrent timers.
class TimingWheelTest : public ::testing::Test {
protected:
  static constexpr std::size_t kWheelSize = 16;
  static constexpr std::size_t kMaxTimers = 32;

  using Wheel = mk::ds::FixedTimingWheel<kWheelSize, kMaxTimers>;
  using handle_t = Wheel::handle_t;
  using tick_t = Wheel::tick_t;
  using cb_t = Wheel::cb_t;

  Wheel wheel_;

  /// Simple counter callback: increments an int pointed to by ctx.
  static void increment_cb(void *ctx) noexcept { ++(*static_cast<int *>(ctx)); }

  /// Callback that records the firing order (pushes ctx value to a vector).
  /// ctx points to a std::pair<std::vector<int>*, int>.
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
// Basic schedule + tick
// =============================================================================

TEST_F(TimingWheelTest, ScheduleAndTickFiresCallback) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 1);
  ASSERT_NE(Wheel::kInvalidHandle, h);
  EXPECT_EQ(1U, wheel_.active_count());

  // Tick once — timer should fire.
  auto fired = wheel_.tick();
  EXPECT_EQ(1U, fired);
  EXPECT_EQ(1, counter);
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelTest, ScheduleWithDelay) {
  int counter = 0;
  (void)wheel_.schedule(increment_cb, &counter, 5);

  // Ticks 0-3: should not fire.
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);

  // Tick 4 (5th tick): should fire.
  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, counter);
}

// =============================================================================
// Multiple timers in same bucket
// =============================================================================

TEST_F(TimingWheelTest, MultipleTimersSameBucket) {
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

TEST_F(TimingWheelTest, CancelPreventsCallback) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 3);

  EXPECT_TRUE(wheel_.cancel(h));
  EXPECT_EQ(0U, wheel_.active_count());

  // Advance past the scheduled tick — callback should NOT fire.
  for (int i = 0; i < 5; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(0, counter);
}

TEST_F(TimingWheelTest, CancelInvalidHandleReturnsFalse) {
  EXPECT_FALSE(wheel_.cancel(Wheel::kInvalidHandle));
}

TEST_F(TimingWheelTest, CancelStaleHandleReturnsFalse) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 1);

  // Fire the timer.
  wheel_.tick();
  EXPECT_EQ(1, counter);

  // Handle is now stale — cancel should return false.
  EXPECT_FALSE(wheel_.cancel(h));
}

TEST_F(TimingWheelTest, DoubleCancelReturnsFalse) {
  int counter = 0;
  auto h = wheel_.schedule(increment_cb, &counter, 5);

  EXPECT_TRUE(wheel_.cancel(h));
  EXPECT_FALSE(wheel_.cancel(h)); // second cancel: stale
}

TEST_F(TimingWheelTest, CancelMiddleOfBucket) {
  // Schedule 3 timers to the same slot, cancel the middle one.
  int c1 = 0;
  int c2 = 0;
  int c3 = 0;
  (void)wheel_.schedule(increment_cb, &c1, 2);
  auto h2 = wheel_.schedule(increment_cb, &c2, 2);
  (void)wheel_.schedule(increment_cb, &c3, 2);

  EXPECT_TRUE(wheel_.cancel(h2));
  EXPECT_EQ(2U, wheel_.active_count());

  // Advance to the slot — only c1 and c3 should fire.
  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(1, c1);
  EXPECT_EQ(0, c2);
  EXPECT_EQ(1, c3);
}

// =============================================================================
// Pool exhaustion
// =============================================================================

TEST_F(TimingWheelTest, PoolExhaustionReturnsInvalidHandle) {
  int counter = 0;

  // Fill the pool.
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter, 1);
    ASSERT_NE(Wheel::kInvalidHandle, h) << "exhausted at i=" << i;
  }
  EXPECT_EQ(kMaxTimers, wheel_.active_count());

  // Next schedule should fail.
  auto h = wheel_.schedule(increment_cb, &counter, 1);
  EXPECT_EQ(Wheel::kInvalidHandle, h);

  // Active count unchanged.
  EXPECT_EQ(kMaxTimers, wheel_.active_count());
}

TEST_F(TimingWheelTest, PoolExhaustionRecoveryAfterFiring) {
  int counter = 0;

  // Fill the pool.
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, 1);
  }

  // Fire all timers — pool should be available again.
  wheel_.tick();
  EXPECT_EQ(static_cast<int>(kMaxTimers), counter);
  EXPECT_EQ(0U, wheel_.active_count());

  // Should be able to schedule again.
  auto h = wheel_.schedule(increment_cb, &counter, 1);
  EXPECT_NE(Wheel::kInvalidHandle, h);
}

// =============================================================================
// Empty bucket tick
// =============================================================================

TEST_F(TimingWheelTest, TickWithEmptyBucketReturnsZero) {
  EXPECT_EQ(0U, wheel_.tick());
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Full rotation (wrap-around)
// =============================================================================

TEST_F(TimingWheelTest, WrapAroundFullRotation) {
  int counter = 0;

  // Schedule a timer at the maximum delay.
  (void)wheel_.schedule(increment_cb, &counter, kWheelSize - 1);

  // Advance WheelSize - 2 ticks — should not fire.
  for (std::size_t i = 0; i < kWheelSize - 2; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);

  // One more tick — should fire.
  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, counter);
}

TEST_F(TimingWheelTest, TimerAfterManyRotations) {
  // Advance several full rotations first, then schedule.
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

TEST_F(TimingWheelTest, ResetClearsAllTimers) {
  int counter = 0;
  for (int i = 1; i < 10; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, (i % (kWheelSize - 1)) + 1);
  }
  EXPECT_EQ(9U, wheel_.active_count());

  wheel_.reset();
  EXPECT_EQ(0U, wheel_.active_count());
  EXPECT_EQ(0U, wheel_.current_tick());

  // Ticking should not fire any callbacks.
  for (std::size_t i = 0; i < kWheelSize; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }
  EXPECT_EQ(0, counter);
}

TEST_F(TimingWheelTest, ResetFreesPoolForReuse) {
  int counter = 0;

  // Fill the pool.
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    (void)wheel_.schedule(increment_cb, &counter, 1);
  }
  EXPECT_EQ(Wheel::kInvalidHandle, wheel_.schedule(increment_cb, &counter, 1));

  // Reset — pool should be fully available again.
  wheel_.reset();
  for (std::size_t i = 0; i < kMaxTimers; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter, 1);
    EXPECT_NE(Wheel::kInvalidHandle, h) << "failed at i=" << i;
  }
}

// =============================================================================
// Observers
// =============================================================================

TEST_F(TimingWheelTest, CurrentTickAdvances) {
  EXPECT_EQ(0U, wheel_.current_tick());
  wheel_.tick();
  EXPECT_EQ(1U, wheel_.current_tick());
  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(3U, wheel_.current_tick());
}

TEST_F(TimingWheelTest, CompileTimeConstants) {
  EXPECT_EQ(kWheelSize, Wheel::wheel_size());
  EXPECT_EQ(kMaxTimers, Wheel::max_timers());
}

// =============================================================================
// Reentrancy: callback schedules new timer to same slot
// =============================================================================

TEST_F(TimingWheelTest, CallbackSchedulesToSameSlot) {
  // A callback that schedules a new timer when it fires.
  // The new timer should NOT fire in the same tick — it should fire
  // on the next rotation.
  struct Ctx {
    Wheel *wheel;
    int fire_count;
    cb_t self_cb;
  };

  const static auto reschedule_cb = [](void *raw) noexcept {
    auto *c = static_cast<Ctx *>(raw);
    ++c->fire_count;
    if (c->fire_count < 3) {
      // Schedule to same delay (same slot on next rotation).
      (void)c->wheel->schedule(c->self_cb, c, Wheel::wheel_size() - 1);
    }
  };

  // We need a non-capturing noexcept function pointer. Use a static wrapper.
  static void (*const wrapper)(void *) noexcept = +reschedule_cb;

  Ctx ctx{.wheel = &wheel_, .fire_count = 0, .self_cb = wrapper};
  (void)wheel_.schedule(wrapper, &ctx, 1);

  // Tick 1: fires the first timer. Callback schedules a new one.
  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(1, ctx.fire_count);
  EXPECT_EQ(1U, wheel_.active_count()); // new timer is scheduled

  // Advance WheelSize - 2 more ticks (the rescheduled timer fires at
  // current_tick + WheelSize - 1 from when it was scheduled).
  for (std::size_t i = 0; i < Wheel::wheel_size() - 2; ++i) {
    EXPECT_EQ(0U, wheel_.tick());
  }

  // Next tick should fire the rescheduled timer.
  EXPECT_EQ(1U, wheel_.tick());
  EXPECT_EQ(2, ctx.fire_count);
}

// =============================================================================
// Reentrancy: callback cancels another timer in the same bucket
// =============================================================================

TEST_F(TimingWheelTest, CallbackCancelsOtherTimerAlreadyDetached) {
  // When tick() detaches the bucket chain and walks it, cancelling
  // a timer in a DIFFERENT bucket should work. Cancelling a timer
  // that was in the SAME bucket but already detached should also be
  // safe (the node is already unlinked, cancel returns false).
  handle_t other_handle = Wheel::kInvalidHandle;

  struct CancelCtx {
    Wheel *wheel;
    handle_t *target;
    int fired;
  };

  const static auto cancel_other_cb = [](void *raw) noexcept {
    auto *c = static_cast<CancelCtx *>(raw);
    ++c->fired;
    // Try to cancel the other timer (in a different bucket).
    c->wheel->cancel(*c->target);
  };

  static void (*const wrapper)(void *) noexcept = +cancel_other_cb;

  int dummy = 0;
  other_handle = wheel_.schedule(increment_cb, &dummy, 5);

  CancelCtx ctx{.wheel = &wheel_, .target = &other_handle, .fired = 0};
  (void)wheel_.schedule(wrapper, &ctx, 1);

  // Tick 1: fires cancel_other_cb, which cancels the timer at delay=5.
  wheel_.tick();
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(0U, wheel_.active_count());

  // The other timer should not fire.
  for (int i = 0; i < 10; ++i) {
    wheel_.tick();
  }
  EXPECT_EQ(0, dummy);
}

// =============================================================================
// Reentrancy: callback cancels a LATER node in the SAME detached chain.
// This is the critical case: A and B are in the same bucket. tick() walks
// A → B → C. A's callback cancels B. Without the in_tick_walk_ guard,
// cancel() would call unlink() on B, breaking the chain (B.next = kNullIdx)
// and orphaning C. With the fix, cancel() marks B as cancelled (cb = nullptr)
// without unlinking, so tick() can skip B and still reach C.
// =============================================================================

TEST_F(TimingWheelTest, CallbackCancelsLaterNodeInSameChain) {
  // Schedule 3 timers to the SAME slot (same delay → same bucket).
  // push_front order: A first, then B, then C.
  // Bucket chain after schedule: C → B → A (push_front reverses order).
  // tick() walks: C → B → A.
  // We'll make C's callback cancel A (a later node in the walk).
  int a_fired = 0;
  int b_fired = 0;

  const handle_t a_handle = wheel_.schedule(increment_cb, &a_fired, 1);
  (void)wheel_.schedule(increment_cb, &b_fired, 1);

  // C's callback cancels A.
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

  // All 3 are in the same bucket (delay=1, same slot).
  EXPECT_EQ(3U, wheel_.active_count());

  // tick(): walks C → B → A.
  // C fires (cancels A), B fires, A is skipped (cancelled).
  const auto fired = wheel_.tick();
  EXPECT_EQ(2U, fired);    // C and B fired; A was cancelled, not fired
  EXPECT_EQ(1, ctx.fired); // C's callback ran
  EXPECT_EQ(1, b_fired);   // B's callback ran
  EXPECT_EQ(0, a_fired);   // A was cancelled before it could fire
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelTest, CallbackCancelsMultipleLaterNodesInSameChain) {
  // Cancel multiple later nodes in the same chain.
  // Chain after schedule (push_front): D → C → B → A.
  // D's callback cancels B and A.
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
  MultiCancelCtx ctx{.wheel = &wheel_,
                     .target_a = a_handle,
                     .target_b = b_handle,
                     .fired = 0};

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
  EXPECT_EQ(2U, fired);    // D and C fired; A and B cancelled
  EXPECT_EQ(1, ctx.fired); // D's callback ran
  EXPECT_EQ(1, c_fired);   // C's callback ran
  EXPECT_EQ(0, b_fired);   // B was cancelled
  EXPECT_EQ(0, a_fired);   // A was cancelled
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Generation counter: reused node slot, old handle is stale
// =============================================================================

TEST_F(TimingWheelTest, GenerationCounterPreventsABA) {
  int c1 = 0;
  int c2 = 0;

  // Schedule and fire a timer — node slot is freed.
  auto h1 = wheel_.schedule(increment_cb, &c1, 1);
  wheel_.tick();
  EXPECT_EQ(1, c1);

  // Schedule a new timer — may reuse the same node slot.
  auto h2 = wheel_.schedule(increment_cb, &c2, 1);

  // Old handle should NOT cancel the new timer (different generation).
  EXPECT_FALSE(wheel_.cancel(h1));

  // New handle should still work.
  EXPECT_TRUE(wheel_.cancel(h2));
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Self-cancel: callback cancels its own timer (must be safe no-op)
// =============================================================================

TEST_F(TimingWheelTest, SelfCancelDuringCallback) {
  // A callback that tries to cancel itself using its own handle.
  // This must be a safe no-op (return false) — the node is already detached
  // from the bucket and in "running" state (cb == nullptr).
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
  EXPECT_FALSE(ctx.cancel_result); // self-cancel should return false
  EXPECT_EQ(0U, wheel_.active_count());
}

TEST_F(TimingWheelTest, SelfCancelDoesNotCorruptOtherTimers) {
  // If a callback self-cancels AND another callback already scheduled
  // a new timer to the same slot, the new timer must NOT be corrupted.
  struct Ctx {
    Wheel *wheel;
    handle_t my_handle;
    int fired;
  };

  const static auto self_cancel_and_schedule_cb = [](void *raw) noexcept {
    auto *c = static_cast<Ctx *>(raw);
    ++c->fired;
    // Self-cancel (should be no-op).
    c->wheel->cancel(c->my_handle);
    // Schedule a new timer to the same slot (current slot + WheelSize - 1
    // will wrap to the same slot on next rotation).
  };

  static void (*const wrapper)(void *) noexcept = +self_cancel_and_schedule_cb;

  int other_counter = 0;
  Ctx ctx{.wheel = &wheel_, .my_handle = Wheel::kInvalidHandle, .fired = 0};
  ctx.my_handle = wheel_.schedule(wrapper, &ctx, 1);

  // Schedule another timer to a different slot.
  (void)wheel_.schedule(increment_cb, &other_counter, 3);

  wheel_.tick(); // fires self-cancel callback
  EXPECT_EQ(1, ctx.fired);
  EXPECT_EQ(1U, wheel_.active_count()); // other timer still alive

  // Ensure the other timer fires correctly (not corrupted).
  wheel_.tick();
  wheel_.tick();
  EXPECT_EQ(1, other_counter);
  EXPECT_EQ(0U, wheel_.active_count());
}

// =============================================================================
// Stress: fill and drain repeatedly
// =============================================================================

TEST_F(TimingWheelTest, FillDrainCycles) {
  int counter = 0;

  for (int cycle = 0; cycle < 10; ++cycle) {
    // Fill pool with timers at various delays.
    for (std::size_t i = 0; i < kMaxTimers; ++i) {
      const tick_t delay = (i % (kWheelSize - 1)) + 1;
      auto h = wheel_.schedule(increment_cb, &counter, delay);
      ASSERT_NE(Wheel::kInvalidHandle, h) << "cycle=" << cycle << " i=" << i;
    }

    // Drain all by ticking through a full rotation.
    for (std::size_t i = 0; i < kWheelSize; ++i) {
      wheel_.tick();
    }

    EXPECT_EQ(0U, wheel_.active_count())
        << "cycle=" << cycle << " has leftover timers";
  }

  // All timers from all cycles should have fired.
  EXPECT_EQ(static_cast<int>(kMaxTimers * 10), counter);
}

// =============================================================================
// Mixed schedule and cancel
// =============================================================================

TEST_F(TimingWheelTest, MixedScheduleCancel) {
  int counter = 0;
  std::vector<handle_t> handles;

  // Schedule 20 timers.
  for (int i = 0; i < 20; ++i) {
    auto h = wheel_.schedule(increment_cb, &counter,
                             static_cast<tick_t>(i % (kWheelSize - 1)) + 1);
    ASSERT_NE(Wheel::kInvalidHandle, h);
    handles.push_back(h);
  }

  // Cancel every other one.
  for (std::size_t i = 0; i < handles.size(); i += 2) {
    EXPECT_TRUE(wheel_.cancel(handles[i]));
  }
  EXPECT_EQ(10U, wheel_.active_count());

  // Drain.
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

using TimingWheelDeathTest = TimingWheelTest;

TEST_F(TimingWheelDeathTest, ScheduleWithDelayZeroAborts) {
  int counter = 0;
  EXPECT_DEATH({ (void)wheel_.schedule(increment_cb, &counter, 0); }, "");
}

TEST_F(TimingWheelDeathTest, ScheduleWithDelayTooLargeAborts) {
  int counter = 0;
  EXPECT_DEATH(
      { (void)wheel_.schedule(increment_cb, &counter, kWheelSize); }, "");
}

TEST_F(TimingWheelDeathTest, ScheduleWithNullCallbackAborts) {
  int counter = 0;
  EXPECT_DEATH({ (void)wheel_.schedule(nullptr, &counter, 1); }, "");
}

#endif // NDEBUG

} // namespace
