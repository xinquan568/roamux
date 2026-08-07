// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/tabs/initial_url_refresh_scheduler.h"

#include <algorithm>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/tick_clock.h"

namespace roamux::tabs {

// static
InitialUrlRefreshScheduler::Params
InitialUrlRefreshScheduler::Params::FromMilliseconds(int interval_ms,
                                                     int min_spacing_ms) {
  // Step 2 — ABSOLUTE validation. An out-of-range value is DISCARDED in favour
  // of the default; it is deliberately NOT clipped to the nearest bound, so a
  // min_spacing_ms of 0 becomes 750 rather than 100. (Clipping 0 to 100 would
  // silently install a floor 7.5x smaller than intended.)
  if (interval_ms < kMinIntervalMs || interval_ms > kMaxIntervalMs) {
    interval_ms = kDefaultIntervalMs;
  }
  if (min_spacing_ms < kMinMinSpacingMs || min_spacing_ms > kMaxMinSpacingMs) {
    min_spacing_ms = kDefaultMinSpacingMs;
  }

  // Step 3 — the single RELATIONAL clamp, and the only step that modifies an
  // otherwise-valid value.
  min_spacing_ms = std::min(min_spacing_ms, interval_ms);

  return Params{base::Milliseconds(interval_ms),
                base::Milliseconds(min_spacing_ms)};
}

InitialUrlRefreshScheduler::InitialUrlRefreshScheduler(
    Delegate* delegate,
    Params params,
    const base::TickClock* tick_clock)
    : delegate_(delegate), params_(params), tick_clock_(tick_clock) {
  CHECK(delegate_);
  CHECK(tick_clock_);
  CHECK_GT(params_.min_spacing, base::TimeDelta());
  CHECK_LE(params_.min_spacing, params_.interval);
}

InitialUrlRefreshScheduler::~InitialUrlRefreshScheduler() = default;

void InitialUrlRefreshScheduler::Start(size_t item_count) {
  CHECK(!running_);
  item_count_ = item_count;
  cursor_ = 0;
  waiting_ = false;
  running_ = true;
  last_start_ = base::TimeTicks();
  StartFromCursor();
}

void InitialUrlRefreshScheduler::NotifyItemSettled(size_t index) {
  // Index-guarded and idempotent: a late signal from a previous item, or one
  // arriving after the run ended, is a no-op.
  if (!running_ || !waiting_ || index != cursor_) {
    return;
  }
  // POST rather than act inline. This is what makes the call safe from inside
  // StartItem (§4.1) — and the posted task re-validates, so a settle raced by
  // Cancel() or by destruction is inert (the weak pointer covers destruction,
  // the re-validation covers cancellation).
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&InitialUrlRefreshScheduler::OnSettled,
                                weak_factory_.GetWeakPtr(), index));
}

void InitialUrlRefreshScheduler::Cancel() {
  if (!running_) {
    return;
  }
  // The item currently loading is left alone — it is already navigating, and
  // aborting would leave a half-loaded page (§3.5 C1).
  Finish();
}

void InitialUrlRefreshScheduler::StartFromCursor() {
  while (running_ && cursor_ < item_count_) {
    // §2.7: while an app-modal dialog is on screen, hold the queue. The cursor
    // does NOT advance, so nothing is skipped when the dialog clears.
    if (delegate_->ShouldDefer()) {
      timer_.Start(FROM_HERE, params_.interval,
                   base::BindOnce(&InitialUrlRefreshScheduler::OnDeferElapsed,
                                  weak_factory_.GetWeakPtr()));
      return;
    }

    // Arm the waiting state BEFORE the out-call: StartItem is explicitly
    // permitted to call NotifyItemSettled synchronously, and that notification
    // must be accepted rather than rejected by a guard that has not been set
    // yet.
    const base::TimeTicks previous_start = last_start_;
    last_start_ = tick_clock_->NowTicks();
    waiting_ = true;

    if (delegate_->StartItem(cursor_)) {
      if (cursor_ + 1 == item_count_) {
        // §3.5: the run reaches its finished state when the last item's load is
        // STARTED, not awaited — a second chord press may begin a fresh run
        // from here (C1b), so waiting out one more interval would be wrong.
        Finish();
      } else {
        timer_.Start(
            FROM_HERE, params_.interval,
            base::BindOnce(&InitialUrlRefreshScheduler::OnIntervalElapsed,
                           weak_factory_.GetWeakPtr()));
      }
      return;
    }

    // Ineligible or gone: advance IMMEDIATELY, in this same turn, and do not
    // consume the min_spacing floor — a skip is not a start.
    waiting_ = false;
    last_start_ = previous_start;
    ++cursor_;
  }

  if (running_) {
    Finish();
  }
}

void InitialUrlRefreshScheduler::OnSettled(size_t index) {
  // Re-validated on delivery, not only at post time.
  if (!running_ || !waiting_ || index != cursor_) {
    return;
  }
  AdvanceAndScheduleNext(last_start_ + params_.min_spacing);
}

void InitialUrlRefreshScheduler::OnIntervalElapsed() {
  if (!running_) {
    return;
  }
  // The relational clamp guarantees min_spacing <= interval, so by the time the
  // interval expires the floor is already satisfied and the next item starts
  // now.
  AdvanceAndScheduleNext(tick_clock_->NowTicks());
}

void InitialUrlRefreshScheduler::OnDeferElapsed() {
  if (!running_) {
    return;
  }
  StartFromCursor();
}

void InitialUrlRefreshScheduler::AdvanceAndScheduleNext(
    base::TimeTicks not_before) {
  waiting_ = false;
  ++cursor_;
  if (cursor_ >= item_count_) {
    Finish();
    return;
  }

  const base::TimeDelta delay = not_before - tick_clock_->NowTicks();
  if (delay.is_positive()) {
    timer_.Start(FROM_HERE, delay,
                 base::BindOnce(&InitialUrlRefreshScheduler::StartFromCursor,
                                weak_factory_.GetWeakPtr()));
    return;
  }
  StartFromCursor();
}

void InitialUrlRefreshScheduler::Finish() {
  CHECK(running_);
  running_ = false;
  waiting_ = false;
  timer_.Stop();
  // Drop any settle already in flight; combined with the running_ check in
  // OnSettled this makes OnRunFinished strictly exactly-once.
  weak_factory_.InvalidateWeakPtrs();
  // Nothing below this line may touch `this`: the delegate is permitted to
  // destroy the scheduler from inside OnRunFinished (roam-269 erases its run
  // from the per-Browser map here).
  delegate_->OnRunFinished();
}

}  // namespace roamux::tabs
