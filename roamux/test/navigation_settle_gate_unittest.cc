// SPDX-License-Identifier: Apache-2.0
// roam-269: the §2.6 Pending/Started gate.
//
// This suite exists because the gate could not be tested through the browser.
// Its motivating scenario — a stale stop from the superseded load arriving while
// our replacement is Pending — is NOT constructible in Chromium: a
// beforeunload-blocked NavigationRequest is installed on the FrameTreeNode
// before its dialog shows and itself keeps the root loading, and
// DidStopLoading fires only on the aggregate transition to NONE. Three
// browsertest attempts failed on exactly that. Extracting the state machine —
// the roam-268 move — makes every path directly assertable instead.

#include "roamux/browser/tabs/navigation_settle_gate.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::tabs {
namespace {

constexpr int64_t kOurNav = 4242;
constexpr int64_t kOtherNav = 99;

// The ordinary asynchronous path: issue, then start, then stop settles.
TEST(NavigationSettleGateTest, AsynchronousStartThenStopSettles) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  EXPECT_TRUE(gate.is_pending());
  EXPECT_FALSE(gate.is_started());

  gate.OnNavigationStarted(kOurNav);
  EXPECT_TRUE(gate.is_started());
  EXPECT_FALSE(gate.is_pending());

  EXPECT_TRUE(gate.OnStopLoading());
  EXPECT_FALSE(gate.is_started());
}

// THE REGRESSION for the bug that shipped past a green CI run: a navigation with
// no beforeunload handler starts SYNCHRONOUSLY inside the load call, before the
// id is known. If that start is dropped, the attempt never becomes Started, no
// stop ever settles, and the completion accelerator is silently dead — every run
// degrades to fixed-interval pacing.
TEST(NavigationSettleGateTest, SynchronousStartInsideLoadCallIsReconciled) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.OnNavigationStarted(kOurNav);  // dispatched during the load call
  EXPECT_FALSE(gate.is_started()) << "cannot be Started before the id is known";

  gate.AttemptIssued(kOurNav);
  EXPECT_TRUE(gate.is_started())
      << "the buffered synchronous start was dropped; the accelerator is dead";
  EXPECT_TRUE(gate.OnStopLoading());
}

// A synchronous start for a DIFFERENT navigation must not be adopted as ours.
TEST(NavigationSettleGateTest, SynchronousStartForAnotherNavigationIsIgnored) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.OnNavigationStarted(kOtherNav);
  gate.AttemptIssued(kOurNav);
  EXPECT_FALSE(gate.is_started());
  EXPECT_TRUE(gate.is_pending());
  EXPECT_FALSE(gate.OnStopLoading()) << "settled on someone else's navigation";
}

// THE GATE ITSELF: a stop arriving while Pending does not settle.
TEST(NavigationSettleGateTest, StopWhilePendingDoesNotSettle) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  ASSERT_TRUE(gate.is_pending());

  EXPECT_FALSE(gate.OnStopLoading())
      << "a stop settled the attempt before our navigation had started";
  EXPECT_TRUE(gate.is_pending()) << "the attempt must remain Pending";

  // ...and once ours starts, the next stop does settle.
  gate.OnNavigationStarted(kOurNav);
  EXPECT_TRUE(gate.OnStopLoading());
}

// A stop dispatched from inside the load call belongs to an earlier load by
// construction — the navigation the call is starting cannot have finished.
TEST(NavigationSettleGateTest, StopInsideLoadCallNeverSettles) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  EXPECT_FALSE(gate.OnStopLoading());
  gate.OnNavigationStarted(kOurNav);
  EXPECT_FALSE(gate.OnStopLoading())
      << "settled inside the load call, before the attempt was even issued";
  gate.AttemptIssued(kOurNav);
  EXPECT_TRUE(gate.is_started());
}

// A start for an unrelated navigation after issue must not promote us.
TEST(NavigationSettleGateTest, UnrelatedStartDoesNotPromote) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  gate.OnNavigationStarted(kOtherNav);
  EXPECT_FALSE(gate.is_started());
  EXPECT_FALSE(gate.OnStopLoading());
}

// §2.6's last clause: the handle went null before Started, so stop treating the
// attempt as Pending — only the interval timer will advance the queue.
TEST(NavigationSettleGateTest, DiscardedBeforeStartClearsPending) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  ASSERT_TRUE(gate.is_pending());

  gate.OnAttemptDiscarded();
  EXPECT_FALSE(gate.is_pending());
  EXPECT_FALSE(gate.is_started());
  EXPECT_FALSE(gate.OnStopLoading());
}

// Discard must NOT tear down an attempt that already started.
TEST(NavigationSettleGateTest, DiscardAfterStartIsIgnored) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  gate.OnNavigationStarted(kOurNav);
  ASSERT_TRUE(gate.is_started());

  gate.OnAttemptDiscarded();
  EXPECT_TRUE(gate.is_started()) << "a started attempt was torn down";
  EXPECT_TRUE(gate.OnStopLoading());
}

// A load that produced no navigation at all leaves nothing Pending.
TEST(NavigationSettleGateTest, AttemptIssuedWithNoNavigationIsInert) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.OnNavigationStarted(kOurNav);
  gate.AttemptIssued(0);
  EXPECT_FALSE(gate.is_pending());
  EXPECT_FALSE(gate.is_started());
  EXPECT_FALSE(gate.OnStopLoading());
}

// A settle is one-shot: a second stop must not settle the same attempt twice,
// which would double-advance the scheduler's cursor.
TEST(NavigationSettleGateTest, SettleIsOneShot) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  gate.OnNavigationStarted(kOurNav);
  ASSERT_TRUE(gate.OnStopLoading());

  EXPECT_FALSE(gate.OnStopLoading()) << "the same attempt settled twice";
  EXPECT_FALSE(gate.is_pending());
}

// State from one item must not leak into the next.
TEST(NavigationSettleGateTest, ConsecutiveAttemptsAreIndependent) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  gate.OnNavigationStarted(kOurNav);
  ASSERT_TRUE(gate.OnStopLoading());

  // Next item: a stop belonging to the PREVIOUS navigation must not settle it.
  gate.BeginAttempt();
  gate.AttemptIssued(kOtherNav);
  EXPECT_TRUE(gate.is_pending());
  EXPECT_FALSE(gate.OnStopLoading());
  gate.OnNavigationStarted(kOurNav);  // the old id
  EXPECT_FALSE(gate.is_started()) << "the previous item's id promoted this one";
  gate.OnNavigationStarted(kOtherNav);
  EXPECT_TRUE(gate.is_started());
}

TEST(NavigationSettleGateTest, ResetAbandonsTheAttemptInFlight) {
  NavigationSettleGate gate;
  gate.BeginAttempt();
  gate.AttemptIssued(kOurNav);
  gate.OnNavigationStarted(kOurNav);
  ASSERT_TRUE(gate.is_started());

  gate.Reset();
  EXPECT_FALSE(gate.is_started());
  EXPECT_FALSE(gate.is_pending());
  EXPECT_FALSE(gate.OnStopLoading()) << "a stop settled a reset attempt";
}

}  // namespace
}  // namespace roamux::tabs
