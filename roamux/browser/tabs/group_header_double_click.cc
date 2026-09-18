// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/tabs/group_header_double_click.h"

#include <algorithm>
#include <utility>

#include "base/auto_reset.h"

namespace roamux::group_header_double_click {

namespace {

// True while a bulk pass runs on this (the UI) thread; a nested call made by
// an observer during a toggle is ignored.
bool g_pass_running = false;

const GroupState* FindGroup(const Snapshot& groups,
                            const tab_groups::TabGroupId& id) {
  for (const GroupState& g : groups) {
    if (g.id == id) {
      return &g;
    }
  }
  return nullptr;
}

}  // namespace

Plan::Plan() = default;
Plan::Plan(const Plan&) = default;
Plan::Plan(Plan&&) = default;
Plan& Plan::operator=(const Plan&) = default;
Plan& Plan::operator=(Plan&&) = default;
Plan::~Plan() = default;

bool EqualExceptClickedFlipped(const Snapshot& before,
                               const Snapshot& now,
                               const tab_groups::TabGroupId& clicked) {
  if (before.size() != now.size()) {
    return false;
  }
  for (const GroupState& b : before) {
    const GroupState* n = FindGroup(now, b.id);
    if (!n) {
      return false;
    }
    const bool expected = b.id == clicked ? !b.collapsed : b.collapsed;
    if (n->collapsed != expected) {
      return false;
    }
  }
  return true;
}

Plan PlanDoubleClickAll(const Snapshot& groups,
                        const tab_groups::TabGroupId& clicked,
                        bool clicked_pre_collapsed) {
  bool all_expanded_before = true;
  for (const GroupState& g : groups) {
    const bool pre = g.id == clicked ? clicked_pre_collapsed : g.collapsed;
    if (pre) {
      all_expanded_before = false;
      break;
    }
  }
  Plan plan;
  plan.target_collapsed = all_expanded_before;
  std::optional<tab_groups::TabGroupId> active_last;
  for (const GroupState& g : groups) {
    if (g.collapsed == plan.target_collapsed) {
      continue;
    }
    if (plan.target_collapsed && g.contains_active_tab) {
      active_last = g.id;
      continue;
    }
    plan.to_toggle.push_back(g.id);
  }
  if (active_last) {
    plan.to_toggle.push_back(*active_last);
  }
  return plan;
}

PassResult RunDoubleClickAll(
    const tab_groups::TabGroupId& clicked,
    bool clicked_pre_collapsed,
    base::FunctionRef<std::optional<Snapshot>()> snapshot,
    base::FunctionRef<bool(const tab_groups::TabGroupId&)> toggle) {
  if (g_pass_running) {
    return PassResult::kIgnoredNested;
  }
  base::AutoReset<bool> running(&g_pass_running, true);

  std::optional<Snapshot> groups = snapshot();
  if (!groups) {
    return PassResult::kStopped;
  }
  const Plan plan = PlanDoubleClickAll(*groups, clicked, clicked_pre_collapsed);
  for (const tab_groups::TabGroupId& id : plan.to_toggle) {
    std::optional<Snapshot> now = snapshot();
    if (!now) {
      return PassResult::kStopped;
    }
    const GroupState* g = FindGroup(*now, id);
    if (!g || g->collapsed == plan.target_collapsed) {
      continue;  // vanished, or already where the pass wants it
    }
    if (!toggle(id)) {
      return PassResult::kStopped;
    }
  }
  return PassResult::kCompleted;
}

GestureState::GestureState() = default;
GestureState::~GestureState() = default;

void GestureState::OnLeftPress(bool double_click_flag,
                               base::TimeTicks press_time,
                               base::TimeDelta max_age,
                               const std::optional<Snapshot>& groups) {
  const bool latch = phase_ == Phase::kRecorded && double_click_flag &&
                     groups.has_value() &&
                     press_time - release_time_ <= max_age &&
                     EqualExceptClickedFlipped(before_, *groups, *clicked_);
  if (latch) {
    phase_ = Phase::kLatched;
  } else {
    Reset();
  }
}

void GestureState::Reset() {
  phase_ = Phase::kIdle;
  clicked_.reset();
  clicked_pre_collapsed_ = false;
  before_.clear();
  release_time_ = base::TimeTicks();
}

void GestureState::RecordSingleToggle(
    const tab_groups::TabGroupId& clicked,
    const std::optional<Snapshot>& before_toggle,
    base::TimeTicks release_time) {
  Reset();
  if (!before_toggle) {
    return;
  }
  const GroupState* g = FindGroup(*before_toggle, clicked);
  if (!g) {
    return;
  }
  phase_ = Phase::kRecorded;
  clicked_ = clicked;
  clicked_pre_collapsed_ = g->collapsed;
  before_ = *before_toggle;
  release_time_ = release_time;
}

std::optional<bool> GestureState::ConsumeOnDoubleClickRelease(
    const tab_groups::TabGroupId& clicked,
    const std::optional<Snapshot>& groups) {
  const bool run = phase_ == Phase::kLatched && clicked_ == clicked &&
                   groups.has_value() &&
                   EqualExceptClickedFlipped(before_, *groups, clicked);
  const bool pre = clicked_pre_collapsed_;
  Reset();  // consumed before any callback runs
  if (!run) {
    return std::nullopt;
  }
  return pre;
}

}  // namespace roamux::group_header_double_click
