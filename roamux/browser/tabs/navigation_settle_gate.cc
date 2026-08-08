// SPDX-License-Identifier: Apache-2.0
// roam-269.
#include "roamux/browser/tabs/navigation_settle_gate.h"

namespace roamux::tabs {

void NavigationSettleGate::BeginAttempt() {
  in_load_call_ = true;
  buffered_start_id_ = 0;
  navigation_id_ = 0;
  started_ = false;
}

void NavigationSettleGate::AttemptIssued(int64_t navigation_id) {
  in_load_call_ = false;
  navigation_id_ = navigation_id;
  if (navigation_id_ == 0) {
    buffered_start_id_ = 0;
    return;
  }
  // A start dispatched synchronously from inside the load call is only
  // identifiable now, once the id has come back.
  if (buffered_start_id_ != 0 && buffered_start_id_ == navigation_id_) {
    started_ = true;
  }
  buffered_start_id_ = 0;
}

void NavigationSettleGate::OnNavigationStarted(int64_t navigation_id) {
  if (in_load_call_) {
    buffered_start_id_ = navigation_id;
    return;
  }
  if (navigation_id_ != 0 && navigation_id == navigation_id_) {
    started_ = true;
  }
}

bool NavigationSettleGate::OnStopLoading() {
  // A stop dispatched from inside the load call cannot belong to the navigation
  // that call is still starting, so it is by definition an earlier load's.
  if (in_load_call_) {
    return false;
  }
  if (!started_) {
    return false;
  }
  started_ = false;
  navigation_id_ = 0;
  return true;
}

void NavigationSettleGate::OnAttemptDiscarded() {
  if (!started_) {
    navigation_id_ = 0;
  }
}

void NavigationSettleGate::Reset() {
  in_load_call_ = false;
  buffered_start_id_ = 0;
  navigation_id_ = 0;
  started_ = false;
}

}  // namespace roamux::tabs
