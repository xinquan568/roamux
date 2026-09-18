// SPDX-License-Identifier: Apache-2.0
// roam-322: double-click a tab group header to collapse or expand ALL groups.
// Pure core (no //chrome deps; lives in the //roamux/browser/tabs source set):
// the per-header gesture state, the target rule, and the bulk pass. The two
// upstream header views (horizontal TabGroupHeader, vertical
// VerticalTabGroupHeaderView — patch 0075) feed it events and supply two
// callbacks that re-resolve the model and controller through weak pointers on
// every call, so nothing here ever holds a model, controller or view pointer.

#ifndef ROAMUX_BROWSER_TABS_GROUP_HEADER_DOUBLE_CLICK_H_
#define ROAMUX_BROWSER_TABS_GROUP_HEADER_DOUBLE_CLICK_H_

#include <optional>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/time/time.h"
#include "components/tab_groups/tab_group_id.h"

namespace roamux::group_header_double_click {

// One group of the window, as the header sees it at a moment.
struct GroupState {
  tab_groups::TabGroupId id;
  bool collapsed = false;
  bool contains_active_tab = false;
};
using Snapshot = std::vector<GroupState>;

// True when `now` holds exactly the groups of `before` (same ids, any order)
// with the same collapsed states, except `clicked`, whose state is flipped —
// the only expected effect of the first click's toggle (an activation move or
// a new ungrouped tab does not change group states). `contains_active_tab` is
// ignored.
bool EqualExceptClickedFlipped(const Snapshot& before,
                               const Snapshot& now,
                               const tab_groups::TabGroupId& clicked);

// The bulk plan: every group ends collapsed iff ALL groups were expanded
// before the gesture. `clicked_pre_collapsed` is the clicked group's state
// before the first click; every other group's pre-gesture state is its state
// in `groups` (valid only after EqualExceptClickedFlipped held). Lists only the
// groups whose state in `groups` differs from the target; on a collapse pass
// the group holding the active tab (at bulk entry) comes last, so activation
// moves at most once during the pass.
struct Plan {
  Plan();
  Plan(const Plan&);
  Plan(Plan&&);
  Plan& operator=(const Plan&);
  Plan& operator=(Plan&&);
  ~Plan();

  bool target_collapsed = false;
  std::vector<tab_groups::TabGroupId> to_toggle;
};
Plan PlanDoubleClickAll(const Snapshot& groups,
                        const tab_groups::TabGroupId& clicked,
                        bool clicked_pre_collapsed);

// Runs the bulk pass. `snapshot()` returns the window's current groups, or
// nullopt when the owner is gone or the window is closing; `toggle(id)` flips
// one group through the strip's own controller and returns false when the
// owner or controller is gone. A fresh snapshot precedes every toggle; a group
// that vanished or already matches the target is skipped; nullopt/false stop
// the pass. A nested call (re-entered from an observer during a toggle) is a
// no-op.
enum class PassResult { kCompleted, kStopped, kIgnoredNested };
PassResult RunDoubleClickAll(
    const tab_groups::TabGroupId& clicked,
    bool clicked_pre_collapsed,
    base::FunctionRef<std::optional<Snapshot>()> snapshot,
    base::FunctionRef<bool(const tab_groups::TabGroupId&)> toggle);

// The window's groups read from a TabStripModel-shaped object (header-only, so
// this pure target keeps no //chrome dependency; the upstream call sites
// instantiate it with TabStripModel, the unit test with a fake). nullopt when
// there is no model, no group model, or the window is closing.
template <typename TabStripModelT>
std::optional<Snapshot> SnapshotFromModel(const TabStripModelT* model) {
  if (!model || model->closing_all() || !model->group_model()) {
    return std::nullopt;
  }
  std::optional<tab_groups::TabGroupId> active_group;
  if (const auto* active = model->GetActiveTab()) {
    active_group = active->GetGroup();
  }
  Snapshot out;
  for (const tab_groups::TabGroupId& id :
       model->group_model()->ListTabGroups()) {
    const auto* group = model->group_model()->GetTabGroup(id);
    if (!group) {
      continue;
    }
    out.push_back(GroupState{id, group->visual_data()->is_collapsed(),
                             active_group == id});
  }
  return out;
}

// Per-header gesture state (a value member of each header view; its lifetime
// is the view's). Idle -> Recorded (a single-click toggle happened) -> Latched
// (the next left press carried the double-click flag, within `max_age` of the
// recorded release, and the window still matched) -> consumed on the release.
class GestureState {
 public:
  GestureState();
  GestureState(const GestureState&) = delete;
  GestureState& operator=(const GestureState&) = delete;
  ~GestureState();

  // Any left press. `groups` is the current window (nullopt: owner invalid).
  void OnLeftPress(bool double_click_flag,
                   base::TimeTicks press_time,
                   base::TimeDelta max_age,
                   const std::optional<Snapshot>& groups);
  // Right-button press, a started drag, an ignored press (editor open), or an
  // invalid owner: back to Idle.
  void Reset();
  // Just BEFORE a single-click toggle of `clicked`: remembers the window as it
  // is before the toggle and the release time.
  void RecordSingleToggle(const tab_groups::TabGroupId& clicked,
                          const std::optional<Snapshot>& before_toggle,
                          base::TimeTicks release_time);
  // A left release carrying the double-click flag. Always consumes (-> Idle).
  // Returns the clicked group's pre-gesture collapsed state when the bulk pass
  // should run (latched, same group, window still matches); nullopt means
  // "treat this release as an ordinary single toggle".
  std::optional<bool> ConsumeOnDoubleClickRelease(
      const tab_groups::TabGroupId& clicked,
      const std::optional<Snapshot>& groups);

  bool is_idle_for_testing() const { return phase_ == Phase::kIdle; }
  bool is_latched_for_testing() const { return phase_ == Phase::kLatched; }

 private:
  enum class Phase { kIdle, kRecorded, kLatched };
  Phase phase_ = Phase::kIdle;
  std::optional<tab_groups::TabGroupId> clicked_;
  bool clicked_pre_collapsed_ = false;
  Snapshot before_;
  base::TimeTicks release_time_;
};

}  // namespace roamux::group_header_double_click

#endif  // ROAMUX_BROWSER_TABS_GROUP_HEADER_DOUBLE_CLICK_H_
