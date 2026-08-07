// SPDX-License-Identifier: Apache-2.0
// roam-268: the pure pacing scheduler (plan §2.3 / §4.1 / §7.3). Twelve cases
// covering the start rule, the min_spacing floor, the interval progress
// backstop, the index-guarded settle signal, re-entrancy, deferral, cancel,
// the fixed-interval degenerate case, and parameter validation.
// (TDD: written RED before the implementation — the header lands as a
// declaration-only contract, so this suite fails at link until roam-268's .cc
// exists.)

#include "roamux/browser/tabs/initial_url_refresh_scheduler.h"

#include <stddef.h>

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::tabs {
namespace {

constexpr base::TimeDelta kInterval = base::Seconds(5);
constexpr base::TimeDelta kMinSpacing = base::Milliseconds(750);

// Records every StartItem call with the time it happened, and lets a test
// script per-index eligibility, deferral, and a synchronous settle.
class FakeDelegate : public InitialUrlRefreshScheduler::Delegate {
 public:
  FakeDelegate(const base::TickClock* clock,
               base::test::TaskEnvironment* task_environment)
      : clock_(clock), task_environment_(task_environment) {}

  // Delegate:
  bool StartItem(size_t index) override {
    starts_.push_back({index, clock_->NowTicks()});
    // A real StartItem does work (roam-269 resolves a TabHandle and calls
    // LoadURLWithParams), so let a test charge that time to the delegate.
    if (!start_latency_.is_zero()) {
      task_environment_->AdvanceClock(start_latency_);
    }
    if (!eligible_.empty() && index < eligible_.size() && !eligible_[index]) {
      return false;
    }
    if (settle_synchronously_) {
      // The §4.1 clause: safe to call from inside StartItem.
      scheduler_->NotifyItemSettled(index);
    }
    return true;
  }
  bool ShouldDefer() override {
    ++defer_checks_;
    return defer_;
  }
  void OnRunFinished() override {
    ++finish_count_;
    finish_time_ = clock_->NowTicks();
    // §4.1 permits the delegate to destroy the scheduler from here (roam-269
    // erases its run from the per-Browser map).
    if (destroy_on_finish_) {
      std::move(destroy_on_finish_).Run();
    }
  }

  struct StartRecord {
    size_t index;
    base::TimeTicks at;
  };

  void set_scheduler(InitialUrlRefreshScheduler* s) { scheduler_ = s; }
  void set_eligible(std::vector<bool> e) { eligible_ = std::move(e); }
  void set_defer(bool d) { defer_ = d; }
  void set_settle_synchronously(bool s) { settle_synchronously_ = s; }
  void set_start_latency(base::TimeDelta d) { start_latency_ = d; }
  void set_destroy_on_finish(base::OnceClosure c) {
    destroy_on_finish_ = std::move(c);
  }

  const std::vector<StartRecord>& starts() const { return starts_; }
  std::vector<size_t> started_indices() const {
    std::vector<size_t> out;
    for (const auto& s : starts_) {
      out.push_back(s.index);
    }
    return out;
  }
  int finish_count() const { return finish_count_; }
  base::TimeTicks finish_time() const { return finish_time_; }
  int defer_checks() const { return defer_checks_; }

 private:
  raw_ptr<const base::TickClock> clock_;
  raw_ptr<base::test::TaskEnvironment> task_environment_;
  raw_ptr<InitialUrlRefreshScheduler> scheduler_ = nullptr;
  std::vector<StartRecord> starts_;
  std::vector<bool> eligible_;
  bool defer_ = false;
  bool settle_synchronously_ = false;
  base::TimeDelta start_latency_;
  base::OnceClosure destroy_on_finish_;
  int finish_count_ = 0;
  int defer_checks_ = 0;
  base::TimeTicks finish_time_;
};

class InitialUrlRefreshSchedulerTest : public testing::Test {
 protected:
  InitialUrlRefreshSchedulerTest()
      : task_environment_(base::test::TaskEnvironment::TimeSource::MOCK_TIME),
        delegate_(task_environment_.GetMockTickClock(), &task_environment_) {}

  // Builds the scheduler with the given knobs and points the fake back at it
  // (needed for the synchronous-settle case).
  InitialUrlRefreshScheduler& MakeScheduler(
      base::TimeDelta interval = kInterval,
      base::TimeDelta min_spacing = kMinSpacing) {
    scheduler_ = std::make_unique<InitialUrlRefreshScheduler>(
        &delegate_, InitialUrlRefreshScheduler::Params{interval, min_spacing},
        task_environment_.GetMockTickClock());
    delegate_.set_scheduler(scheduler_.get());
    return *scheduler_;
  }

  // The fake holds a back-pointer to the scheduler (for the synchronous-settle
  // case) while the scheduler holds the fake as its Delegate. Break the cycle
  // from the fake's side FIRST: destroying the scheduler while a raw_ptr still
  // addresses it is what BackupRefPtr reports as a dangling pointer.
  void TearDown() override {
    delegate_.set_scheduler(nullptr);
    scheduler_.reset();
  }

  base::TimeTicks Now() const {
    return task_environment_.GetMockTickClock()->NowTicks();
  }

  base::test::TaskEnvironment task_environment_;
  FakeDelegate delegate_;
  std::unique_ptr<InitialUrlRefreshScheduler> scheduler_;
};

// 1. Items start in snapshot order; item 0 starts at t = 0, bypassing the
//    floor.
TEST_F(InitialUrlRefreshSchedulerTest, StartsInOrderAndFirstItemBypassesFloor) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();

  scheduler.Start(3);

  ASSERT_EQ(1u, delegate_.starts().size());
  EXPECT_EQ(0u, delegate_.starts()[0].index);
  EXPECT_EQ(t0, delegate_.starts()[0].at) << "item 0 must start at t=0";

  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_EQ((std::vector<size_t>{0u, 1u, 2u}), delegate_.started_indices());
}

// 2. At most one item is ever waited on: between two consecutive starts the
//    scheduler never issues a third.
TEST_F(InitialUrlRefreshSchedulerTest, AtMostOneItemWaitedOnAtATime) {
  auto& scheduler = MakeScheduler();
  scheduler.Start(4);
  EXPECT_EQ(1u, delegate_.starts().size());

  // Just under one interval: still exactly one start outstanding.
  task_environment_.FastForwardBy(kInterval - base::Milliseconds(1));
  EXPECT_EQ(1u, delegate_.starts().size());

  task_environment_.FastForwardBy(base::Milliseconds(1));
  EXPECT_EQ(2u, delegate_.starts().size());

  task_environment_.FastForwardBy(kInterval - base::Milliseconds(1));
  EXPECT_EQ(2u, delegate_.starts().size());
}

// 3. A settle before `interval` advances early, at
//    max(now, last_start + min_spacing).
TEST_F(InitialUrlRefreshSchedulerTest, SettleAdvancesEarlyAtMaxNowOrFloor) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  scheduler.Start(2);

  // Settle well after the floor but well before the interval: the next start
  // is immediate (now > last_start + min_spacing).
  task_environment_.FastForwardBy(base::Seconds(2));
  scheduler.NotifyItemSettled(0);
  task_environment_.RunUntilIdle();

  ASSERT_EQ(2u, delegate_.starts().size());
  EXPECT_EQ(1u, delegate_.starts()[1].index);
  EXPECT_EQ(t0 + base::Seconds(2), delegate_.starts()[1].at);
}

// 4. THE PROGRESS GUARANTEE (§2.3): no settle ever arrives, yet the interval
//    timer still starts all N — and the run finishes at the LAST START, not one
//    interval later (§3.5: kFinished when the last load is started, not
//    awaited).
TEST_F(InitialUrlRefreshSchedulerTest,
       IntervalTimerStartsAllItemsWithoutSettles) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  scheduler.Start(4);

  task_environment_.FastForwardBy(kInterval * 4);

  EXPECT_EQ((std::vector<size_t>{0u, 1u, 2u, 3u}), delegate_.started_indices());
  EXPECT_EQ(t0, delegate_.starts()[0].at);
  EXPECT_EQ(t0 + kInterval, delegate_.starts()[1].at);
  EXPECT_EQ(t0 + kInterval * 2, delegate_.starts()[2].at);
  EXPECT_EQ(t0 + kInterval * 3, delegate_.starts()[3].at);

  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_EQ(t0 + kInterval * 3, delegate_.finish_time())
      << "the run finishes when the LAST item STARTS, not after it settles";
}

// 5. An immediate settle is floored, and the floor cannot be compounded away:
//    across N items the run still takes (N-1) * min_spacing.
TEST_F(InitialUrlRefreshSchedulerTest,
       ImmediateSettlesAreFlooredAndNotCompounded) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  delegate_.set_settle_synchronously(true);

  constexpr size_t kN = 5;
  scheduler.Start(kN);
  task_environment_.FastForwardBy(kMinSpacing * kN);

  ASSERT_EQ(kN, delegate_.starts().size());
  for (size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(i, delegate_.starts()[i].index);
    EXPECT_EQ(t0 + kMinSpacing * i, delegate_.starts()[i].at)
        << "start " << i << " must be floored off the PREVIOUS START";
  }
  EXPECT_EQ(t0 + kMinSpacing * (kN - 1), delegate_.starts().back().at);
}

// 6. A stale index settle (a previous item settling late) is a no-op and does
//    not advance the queue.
TEST_F(InitialUrlRefreshSchedulerTest, StaleIndexSettleIsANoOp) {
  auto& scheduler = MakeScheduler();
  scheduler.Start(3);

  // Advance to item 1 via the interval timer.
  task_environment_.FastForwardBy(kInterval);
  ASSERT_EQ(2u, delegate_.starts().size());

  // Item 0 settles late. Nothing should move.
  scheduler.NotifyItemSettled(0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(2u, delegate_.starts().size());

  // And an out-of-range index is equally inert.
  scheduler.NotifyItemSettled(99);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(2u, delegate_.starts().size());
}

// 7. NotifyItemSettled called synchronously from inside StartItem does not
//    re-enter the scheduler and still advances. (The guard must not reject the
//    in-call notification — the regression that made this case unpassable.)
TEST_F(InitialUrlRefreshSchedulerTest,
       SynchronousSettleFromStartItemDoesNotReenter) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  delegate_.set_settle_synchronously(true);

  scheduler.Start(3);
  // Exactly one start has happened so far: the settle posted, it did not
  // recurse into a second StartItem within the same call.
  EXPECT_EQ(1u, delegate_.starts().size());

  task_environment_.FastForwardBy(kMinSpacing * 3);
  EXPECT_EQ((std::vector<size_t>{0u, 1u, 2u}), delegate_.started_indices());
  EXPECT_EQ(t0 + kMinSpacing, delegate_.starts()[1].at);
}

// 8. StartItem returning false (ineligible/gone) advances IMMEDIATELY, without
//    consuming the min_spacing floor.
TEST_F(InitialUrlRefreshSchedulerTest,
       IneligibleItemAdvancesImmediatelyWithoutFloor) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  // Items 0, 1, 2 are gone; item 3 is real.
  delegate_.set_eligible({false, false, false, true});

  scheduler.Start(4);

  // All four StartItem calls happened in the same turn, at t = 0.
  ASSERT_EQ(4u, delegate_.starts().size());
  for (const auto& s : delegate_.starts()) {
    EXPECT_EQ(t0, s.at) << "a skip must not consume the floor";
  }
  EXPECT_EQ((std::vector<size_t>{0u, 1u, 2u, 3u}), delegate_.started_indices());
}

// 8, continued: a queue where EVERY item is skipped finishes in the first turn,
// with no timer ever armed. Same property as above (a skip costs nothing), at
// the boundary where the run never starts anything at all.
TEST_F(InitialUrlRefreshSchedulerTest, AllSkippedQueueFinishesImmediately) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  delegate_.set_eligible({false, false, false});

  scheduler.Start(3);

  EXPECT_EQ(3u, delegate_.starts().size());
  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_EQ(t0, delegate_.finish_time());
  EXPECT_FALSE(scheduler.is_running());

  // Nothing was scheduled, so time passing changes nothing.
  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_EQ(3u, delegate_.starts().size());
  EXPECT_EQ(1, delegate_.finish_count());
}

// 8c. An EMPTY run finishes immediately and exactly once (the Start(0) contract
//     on the header).
TEST_F(InitialUrlRefreshSchedulerTest,
       EmptyRunFinishesImmediatelyAndExactlyOnce) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();

  scheduler.Start(0);

  EXPECT_TRUE(delegate_.starts().empty());
  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_EQ(t0, delegate_.finish_time());
  EXPECT_FALSE(scheduler.is_running());

  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_EQ(1, delegate_.finish_count());
}

// 8d. The interval backstop is anchored to the previous START, not to the
//     moment StartItem returned. A delegate that consumes time must not push
//     subsequent starts later than the rule allows, and that error must not
//     compound across the run. (§2.3: "`interval` after the previous start".)
TEST_F(InitialUrlRefreshSchedulerTest, SlowDelegateDoesNotPushOutTheInterval) {
  auto& scheduler = MakeScheduler();
  const base::TimeTicks t0 = Now();
  constexpr base::TimeDelta kLatency = base::Seconds(2);
  delegate_.set_start_latency(kLatency);

  scheduler.Start(3);
  task_environment_.FastForwardBy(kInterval * 3);

  ASSERT_EQ(3u, delegate_.starts().size());
  // Anchored: 0s, 5s, 10s. Anchoring to StartItem's RETURN instead would give
  // 0s, 7s, 14s — the 2s latency charged on top of every interval.
  EXPECT_EQ(t0, delegate_.starts()[0].at);
  EXPECT_EQ(t0 + kInterval, delegate_.starts()[1].at);
  EXPECT_EQ(t0 + kInterval * 2, delegate_.starts()[2].at);
}

// 8e. The delegate may destroy the scheduler from inside OnRunFinished — the
//     §4.1 clause roam-269 relies on when it erases its run from the
//     per-Browser map.
TEST_F(InitialUrlRefreshSchedulerTest,
       DelegateMayDestroySchedulerFromOnRunFinished) {
  auto& scheduler = MakeScheduler();
  delegate_.set_destroy_on_finish(base::BindLambdaForTesting([this] {
    delegate_.set_scheduler(nullptr);
    scheduler_.reset();
  }));

  // A single item: the run finishes as soon as that item's load STARTS.
  scheduler.Start(1);

  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_FALSE(scheduler_) << "the delegate destroyed it from OnRunFinished";
  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_EQ(1, delegate_.finish_count());
}

// 8f. A synchronous settle on the FINAL item still finishes exactly once: the
//     run is already finished when the posted settle would have been delivered.
TEST_F(InitialUrlRefreshSchedulerTest,
       FinalItemSynchronousSettleFinishesExactlyOnce) {
  auto& scheduler = MakeScheduler();
  delegate_.set_settle_synchronously(true);

  scheduler.Start(2);
  task_environment_.FastForwardBy(kInterval * 3);

  EXPECT_EQ(2u, delegate_.starts().size());
  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_FALSE(scheduler.is_running());
}

// 9. ShouldDefer() true holds the queue; starts resume once it clears, with no
//    item skipped.
TEST_F(InitialUrlRefreshSchedulerTest, DeferHoldsTheQueueAndSkipsNothing) {
  auto& scheduler = MakeScheduler();
  delegate_.set_defer(true);

  scheduler.Start(3);
  EXPECT_TRUE(delegate_.starts().empty()) << "deferral must hold the queue";

  // Still deferring after several intervals — and still nothing skipped.
  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_TRUE(delegate_.starts().empty());

  delegate_.set_defer(false);
  task_environment_.FastForwardBy(kInterval);
  EXPECT_FALSE(delegate_.starts().empty());
  EXPECT_EQ(0u, delegate_.starts()[0].index) << "item 0 must not be skipped";

  task_environment_.FastForwardBy(kInterval * 3);
  EXPECT_EQ((std::vector<size_t>{0u, 1u, 2u}), delegate_.started_indices());
}

// 10. Cancel drops the queue, leaves the current item alone, and finishes
//     exactly once; a late settle after cancel is harmless — including one
//     posted before the cancel and delivered after it.
TEST_F(InitialUrlRefreshSchedulerTest,
       CancelDropsQueueAndLateSettleIsHarmless) {
  auto& scheduler = MakeScheduler();
  scheduler.Start(4);
  ASSERT_EQ(1u, delegate_.starts().size());

  // A settle is issued (it posts) and then the run is cancelled before the
  // posted task runs.
  scheduler.NotifyItemSettled(0);
  scheduler.Cancel();
  task_environment_.RunUntilIdle();

  EXPECT_FALSE(scheduler.is_running());
  EXPECT_EQ(1, delegate_.finish_count());
  EXPECT_EQ(1u, delegate_.starts().size()) << "the queue must be dropped";

  // Nothing further, ever.
  task_environment_.FastForwardBy(kInterval * 5);
  scheduler.NotifyItemSettled(0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1u, delegate_.starts().size());
  EXPECT_EQ(1, delegate_.finish_count()) << "OnRunFinished fires exactly once";
}

// 11. min_spacing == interval reproduces exact fixed-interval timing,
//     regardless of settle notifications.
TEST_F(InitialUrlRefreshSchedulerTest,
       MinSpacingEqualToIntervalIsFixedInterval) {
  auto& scheduler = MakeScheduler(kInterval, kInterval);
  const base::TimeTicks t0 = Now();
  delegate_.set_settle_synchronously(true);

  scheduler.Start(4);
  task_environment_.FastForwardBy(kInterval * 4);

  ASSERT_EQ(4u, delegate_.starts().size());
  for (size_t i = 0; i < 4u; ++i) {
    EXPECT_EQ(t0 + kInterval * i, delegate_.starts()[i].at)
        << "start " << i << " must be exactly on the fixed interval";
  }
}

// 12. Parameter validation, following §7.3's three steps exactly.
TEST(InitialUrlRefreshSchedulerParamsTest, ValidationFollowsTheThreeStepOrder) {
  using Params = InitialUrlRefreshScheduler::Params;
  constexpr auto kDefaultInterval =
      base::Milliseconds(InitialUrlRefreshScheduler::kDefaultIntervalMs);
  constexpr auto kDefaultMinSpacing =
      base::Milliseconds(InitialUrlRefreshScheduler::kDefaultMinSpacingMs);

  // Both in range: taken as given.
  {
    const Params p = Params::FromMilliseconds(8000, 500);
    EXPECT_EQ(base::Milliseconds(8000), p.interval);
    EXPECT_EQ(base::Milliseconds(500), p.min_spacing);
  }

  // min_spacing_ms = 0 falls back to the DEFAULT (750), NOT clipped to the
  // 100 ms bound. Zero would permit the zero-delay burst the floor prevents.
  {
    const Params p = Params::FromMilliseconds(
        InitialUrlRefreshScheduler::kDefaultIntervalMs, 0);
    EXPECT_EQ(kDefaultMinSpacing, p.min_spacing);
  }

  // Negative likewise falls back to the default.
  {
    const Params p = Params::FromMilliseconds(
        InitialUrlRefreshScheduler::kDefaultIntervalMs, -1);
    EXPECT_EQ(kDefaultMinSpacing, p.min_spacing);
  }

  // interval_ms = 100 is below the 250 ms bound, so it falls back to 5000 —
  // again a default, not a clip to 250.
  {
    const Params p = Params::FromMilliseconds(
        100, InitialUrlRefreshScheduler::kDefaultMinSpacingMs);
    EXPECT_EQ(kDefaultInterval, p.interval);
  }

  // Above-range values fall back too.
  {
    const Params p = Params::FromMilliseconds(
        InitialUrlRefreshScheduler::kMaxIntervalMs + 1,
        InitialUrlRefreshScheduler::kMaxMinSpacingMs + 1);
    EXPECT_EQ(kDefaultInterval, p.interval);
    EXPECT_EQ(kDefaultMinSpacing, p.min_spacing);
  }

  // Step 3, the only step that modifies an otherwise-valid value: a valid
  // min_spacing above a valid interval clamps DOWN to the interval.
  {
    const Params p = Params::FromMilliseconds(1000, 2000);
    EXPECT_EQ(base::Milliseconds(1000), p.interval);
    EXPECT_EQ(base::Milliseconds(1000), p.min_spacing);
  }

  // The bounds are INCLUSIVE. Pin every endpoint exactly, so an implementation
  // that made any of them exclusive fails here rather than passing the broad
  // invariants below.
  {
    const Params p =
        Params::FromMilliseconds(InitialUrlRefreshScheduler::kMinIntervalMs,
                                 InitialUrlRefreshScheduler::kMinMinSpacingMs);
    EXPECT_EQ(base::Milliseconds(InitialUrlRefreshScheduler::kMinIntervalMs),
              p.interval);
    EXPECT_EQ(base::Milliseconds(InitialUrlRefreshScheduler::kMinMinSpacingMs),
              p.min_spacing);
  }
  {
    const Params p =
        Params::FromMilliseconds(InitialUrlRefreshScheduler::kMaxIntervalMs,
                                 InitialUrlRefreshScheduler::kMaxMinSpacingMs);
    EXPECT_EQ(base::Milliseconds(InitialUrlRefreshScheduler::kMaxIntervalMs),
              p.interval);
    // Both at max: equal, so the relational clamp is a no-op.
    EXPECT_EQ(base::Milliseconds(InitialUrlRefreshScheduler::kMaxMinSpacingMs),
              p.min_spacing);
  }
  // Just outside each inclusive bound falls back to the default.
  {
    const Params p = Params::FromMilliseconds(
        InitialUrlRefreshScheduler::kMinIntervalMs - 1,
        InitialUrlRefreshScheduler::kMinMinSpacingMs - 1);
    EXPECT_EQ(kDefaultInterval, p.interval);
    EXPECT_EQ(kDefaultMinSpacing, p.min_spacing);
  }

  // Mixed cases that pin the ORDER of the two steps: absolute validation runs
  // first and yields defaults, and only then does the relational clamp apply.
  {
    // interval 100 is invalid -> default 5000; min_spacing 1000 is valid and
    // below 5000, so it survives unclamped.
    const Params p = Params::FromMilliseconds(100, 1000);
    EXPECT_EQ(kDefaultInterval, p.interval);
    EXPECT_EQ(base::Milliseconds(1000), p.min_spacing);
  }
  {
    // min_spacing 0 is invalid -> default 750; interval 250 is valid, and the
    // relational clamp then pulls 750 down to 250. Clipping 0 to the 100 ms
    // bound instead would have produced 100 here.
    const Params p = Params::FromMilliseconds(250, 0);
    EXPECT_EQ(base::Milliseconds(250), p.interval);
    EXPECT_EQ(base::Milliseconds(250), p.min_spacing);
  }

  // No configuration yields a zero-delay burst or a permanently stalled queue.
  for (const int interval_ms : {-1, 0, 1, 100, 250, 5000, 600000, 600001}) {
    for (const int min_spacing_ms : {-1, 0, 1, 99, 100, 750, 600000, 600001}) {
      const Params p = Params::FromMilliseconds(interval_ms, min_spacing_ms);
      EXPECT_GT(p.min_spacing, base::TimeDelta())
          << interval_ms << "/" << min_spacing_ms;
      EXPECT_LE(p.min_spacing, p.interval)
          << interval_ms << "/" << min_spacing_ms;
      EXPECT_LE(p.interval,
                base::Milliseconds(InitialUrlRefreshScheduler::kMaxIntervalMs))
          << interval_ms << "/" << min_spacing_ms;
    }
  }
}

}  // namespace
}  // namespace roamux::tabs
