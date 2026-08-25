// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/tabs/new_tab_placement.h"

namespace roamux::tabs {

NewTabPlacement ComputeNewTabPlacement(
    NewTabPosition mode,
    int tab_count,
    int active_index,
    std::optional<tab_groups::TabGroupId> active_group,
    bool upstream_new_tab_adds_to_active_group) {
  NewTabPlacement placement;  // {-1, nullopt}: a plain append.
  if (active_index < 0 || active_index >= tab_count) {
    return placement;
  }
  switch (mode) {
    case NewTabPosition::kEndOfStrip:
      break;
    case NewTabPosition::kEndOfActiveGroup:
      if (upstream_new_tab_adds_to_active_group) {
        placement.group = active_group;
      }
      break;
    case NewTabPosition::kAfterActiveTab:
      placement.index = active_index + 1;
      placement.group = active_group;
      break;
  }
  return placement;
}

}  // namespace roamux::tabs
