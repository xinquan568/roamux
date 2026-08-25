// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_TABS_NEW_TAB_PLACEMENT_H_
#define ROAMUX_BROWSER_TABS_NEW_TAB_PLACEMENT_H_

#include <optional>

#include "components/tab_groups/tab_group_id.h"
#include "roamux/common/new_tab_position.h"

namespace roamux::tabs {

// What chrome::NewTab() asks TabStripModel for: the (index, group) pair
// handed to chrome::AddAndReturnTabAt. `index` -1 means "append"; `group`
// nullopt means "no group requested".
struct NewTabPlacement {
  int index = -1;
  std::optional<tab_groups::TabGroupId> group;
};

// roam-277: the PURE placement table for the explicit new-tab routes. Takes
// primitives only (no TabStripModel*, no //chrome) so it stays inside the
// no-//chrome contract of //roamux/browser/tabs and is unit-tested in
// roamux_unittests.
//
//   kEndOfStrip        -> {-1, nullopt}
//   kEndOfActiveGroup  -> {-1, upstream ? active_group : nullopt}
//                         (the stock patch-0067 request, so an explicit
//                         upstream opt-out via kNewTabAddsToActiveGroup keeps
//                         meaning strip end)
//   kAfterActiveTab    -> {active_index + 1, active_group}
//                         (the group is passed EXPLICITLY — nullopt for an
//                         ungrouped active tab keeps the new tab ungrouped
//                         even when active+1 starts a group; the upstream
//                         flag is not consulted: an adjacent in-group tab
//                         cannot be ungrouped)
//
// An active index outside [0, tab_count) (empty strip, no active tab) yields
// {-1, nullopt} in every mode. Pinned / split / group-range clamping is
// deliberately NOT replicated here — TabStripModel::AddTab and
// ConstrainInsertionIndex own it (ConstrainInsertionIndex moves a request
// inside the pinned run to the first unpinned slot;
// InsertionBreaksSplitContiguity pushes it past a split; the group clamp keeps
// it inside the group's range).
NewTabPlacement ComputeNewTabPlacement(
    NewTabPosition mode,
    int tab_count,
    int active_index,
    std::optional<tab_groups::TabGroupId> active_group,
    bool upstream_new_tab_adds_to_active_group);

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_NEW_TAB_PLACEMENT_H_
