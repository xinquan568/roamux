// SPDX-License-Identifier: Apache-2.0
// roam-269: the §2.6 Pending/Started completion gate, extracted from
// InitialUrlRefreshRun so it can be tested without a browser.
#ifndef ROAMUX_BROWSER_TABS_NAVIGATION_SETTLE_GATE_H_
#define ROAMUX_BROWSER_TABS_NAVIGATION_SETTLE_GATE_H_

#include <stdint.h>

namespace roamux::tabs {

// Decides whether a "this tab stopped loading" signal may settle the refresh
// attempt we issued for that tab.
//
// PURE by construction: it lives in the //roamux/browser/tabs source_set and
// speaks only in navigation ids and booleans — no content::, no //chrome. That
// is the same move roam-268 made for the pacing scheduler, and for the same
// reason: the interesting behaviour here is a small state machine whose every
// path is worth pinning, and browser bring-up is both unnecessary and (see
// below) incapable of reaching the case that motivates it.
//
// THE RULE (plan §2.6). An attempt is:
//   * Pending  — from the moment the load is issued until a primary-main-frame
//                navigation starts carrying OUR navigation id;
//   * Started  — from then on.
// A stop settles the attempt ONLY when it is Started.
//
// Why the gate exists, stated accurately. The design's headline justification —
// a stale stop from the load already in flight when the tab was dequeued
// arriving while we are Pending — is NOT reachable through Chromium's
// navigation semantics: a beforeunload-blocked NavigationRequest is installed on
// the FrameTreeNode before its dialog shows and itself keeps the root loading,
// and DidStopLoading fires only on the aggregate transition to NONE. What the
// gate does still constrain, per §2.6's remaining clauses, is every OTHER stop
// that can arrive while we are Pending: a stop belonging to a same-document
// navigation, a redirect, or a later navigation replacing ours. Each of those
// can cause at most one early advance, floored by min_spacing.
class NavigationSettleGate {
 public:
  NavigationSettleGate() = default;

  // Call immediately BEFORE issuing the load. Observation must already be
  // active: a browser-initiated navigation with no beforeunload handler starts
  // SYNCHRONOUSLY inside the load call, and a start seen then is buffered here
  // because the id to match against has not been returned yet. Getting this
  // ordering wrong is not cosmetic — it silently disables the completion
  // accelerator, since no attempt ever becomes Started and so none ever settles.
  void BeginAttempt();

  // Call immediately AFTER the load returns, with the navigation id it produced
  // (or 0 if it produced none). Reconciles any start buffered during the call.
  void AttemptIssued(int64_t navigation_id);

  // A primary-main-frame navigation started.
  void OnNavigationStarted(int64_t navigation_id);

  // The tab stopped loading. Returns true iff this settles our attempt; the
  // caller notifies the scheduler only then.
  bool OnStopLoading();

  // Our navigation handle went null before the attempt started — it was
  // discarded or replaced. The attempt stops being Pending; only the interval
  // timer will advance the queue (§2.6's last clause).
  void OnAttemptDiscarded();

  // Abandon any attempt in flight.
  void Reset();

  bool is_started() const { return started_; }
  // Issued, not yet started, and not discarded.
  bool is_pending() const { return navigation_id_ != 0 && !started_; }

 private:
  // True between BeginAttempt() and AttemptIssued() — i.e. inside the load call.
  bool in_load_call_ = false;
  // A start observed during the load call, before we knew which id to expect.
  int64_t buffered_start_id_ = 0;
  int64_t navigation_id_ = 0;
  bool started_ = false;
};

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_NAVIGATION_SETTLE_GATE_H_
