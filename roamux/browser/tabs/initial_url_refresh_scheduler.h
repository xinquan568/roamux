// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_SCHEDULER_H_
#define ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_SCHEDULER_H_

#include <stddef.h>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"

namespace base {
class TickClock;
}

namespace roamux::tabs {

// The pacing engine for "refresh all tabs to their initial URLs" (roam-268,
// plan §2.3/§4.1). PURE: it lives in the //roamux/browser/tabs source_set, so
// it must stay free of //chrome and content:: — it works over opaque item
// indices and learns about the world only through its Delegate. That is what
// makes the whole algorithm testable under MOCK_TIME with no browser bring-up;
// the chrome-facing InitialUrlRefreshRun (roam-269) implements the Delegate.
//
// THE RULE (§2.3):
//   Start the next item when the previous one settles, or `interval` after the
//   previous start — whichever comes first — but never sooner than
//   `min_spacing` after the previous start.
//
// At most one item is ever waited on. The `interval` timer is the backstop that
// makes progress guaranteed: no item can wedge the run behind it.
class InitialUrlRefreshScheduler {
 public:
  // §7.3 defaults and absolute-validation bounds. min_spacing has a POSITIVE
  // minimum on purpose: zero would permit the zero-delay burst the floor
  // exists to prevent.
  static constexpr int kDefaultIntervalMs = 5000;
  static constexpr int kDefaultMinSpacingMs = 750;
  static constexpr int kMinIntervalMs = 250;
  static constexpr int kMaxIntervalMs = 600000;
  static constexpr int kMinMinSpacingMs = 100;
  static constexpr int kMaxMinSpacingMs = 600000;

  struct Params {
    base::TimeDelta interval;
    base::TimeDelta min_spacing;

    // §7.3's fixed three-step order, which is why "falls back" and "is clamped"
    // never apply to the same value ambiguously:
    //   1. read each value independently;
    //   2. ABSOLUTE-VALIDATE — a value outside its own range is DISCARDED and
    //      the DEFAULT is used in its place. It is NOT clipped to the nearest
    //      bound (min_spacing_ms of 0 becomes 750, never 100);
    //   3. RELATIONAL CLAMP — min_spacing = min(min_spacing, interval). This is
    //      the only step that modifies an otherwise-valid value.
    static Params FromMilliseconds(int interval_ms, int min_spacing_ms);
  };

  class Delegate {
   public:
    // Start item `index`. Returns true if a load actually began; false if the
    // item was ineligible or gone, in which case the scheduler advances
    // IMMEDIATELY and the skip does NOT consume the min_spacing floor.
    // Must not re-enter the scheduler other than via NotifyItemSettled.
    virtual bool StartItem(size_t index) = 0;

    // True while an app-modal dialog is on screen (§2.7): defer by `interval`
    // and re-check. Nothing is skipped while deferring.
    virtual bool ShouldDefer() = 0;

    // Called exactly once per run, when the queue empties (the last item's load
    // is STARTED, not awaited — §3.5) or on Cancel(). The scheduler touches no
    // member after this returns, so the delegate may destroy it from here.
    virtual void OnRunFinished() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  InitialUrlRefreshScheduler(Delegate* delegate,
                             Params params,
                             const base::TickClock* tick_clock);
  InitialUrlRefreshScheduler(const InitialUrlRefreshScheduler&) = delete;
  InitialUrlRefreshScheduler& operator=(const InitialUrlRefreshScheduler&) =
      delete;
  ~InitialUrlRefreshScheduler();

  // Runs `item_count` items in index order. Item 0 starts immediately, at
  // t = 0, bypassing the floor. An empty run finishes at once.
  void Start(size_t item_count);

  // The single asynchronous input: item `index` stopped loading (or its backing
  // object went away). Idempotent, and ignored unless `index` is still the
  // current item — a late signal from a previous item is a no-op. Safe to call
  // synchronously from inside StartItem: it posts rather than acting inline,
  // and the posted delivery re-validates, so a settle raced by Cancel() or by
  // destruction is inert.
  void NotifyItemSettled(size_t index);

  // Drops the pending queue and finishes. The item currently loading is left
  // alone — it is already navigating.
  void Cancel();

  bool is_running() const { return running_; }

 private:
  void StartFromCursor();
  void OnSettled(size_t index);
  void OnIntervalElapsed();
  void OnDeferElapsed();
  void AdvanceAndScheduleNext(base::TimeTicks not_before);
  void Finish();

  const raw_ptr<Delegate> delegate_;
  const Params params_;
  const raw_ptr<const base::TickClock> tick_clock_;

  base::OneShotTimer timer_;
  size_t item_count_ = 0;
  size_t cursor_ = 0;
  bool running_ = false;
  bool waiting_ = false;
  base::TimeTicks last_start_;

  base::WeakPtrFactory<InitialUrlRefreshScheduler> weak_factory_{this};
};

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_SCHEDULER_H_
