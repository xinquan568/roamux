// SPDX-License-Identifier: Apache-2.0
#include "roamux/common/tab_strip_placement.h"

#include <algorithm>

#include "base/feature_list.h"
#include "components/prefs/pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"

namespace roamux {

TabStripPlacement GetTabStripPlacement(const PrefService* pref_service) {
  if (!pref_service ||
      !base::FeatureList::IsEnabled(features::kTabStripPosition)) {
    return TabStripPlacement::kTop;
  }
  const int stored = pref_service->GetInteger(prefs::kTabStripPosition);
  if (stored < static_cast<int>(TabStripPlacement::kTop) ||
      stored > static_cast<int>(TabStripPlacement::kRight)) {
    return TabStripPlacement::kTop;
  }
  return static_cast<TabStripPlacement>(stored);
}

void SetTabStripPlacement(PrefService* pref_service,
                          TabStripPlacement placement) {
  if (!pref_service) {
    return;
  }
  pref_service->SetInteger(prefs::kTabStripPosition,
                           static_cast<int>(placement));
}

bool ShouldDisplayVerticalTabsForPlacement(const PrefService* pref_service) {
  const TabStripPlacement placement = GetTabStripPlacement(pref_service);
  return placement == TabStripPlacement::kLeft ||
         placement == TabStripPlacement::kRight;
}

bool ShouldDockVerticalTabStripRight(const PrefService* pref_service) {
  // roam-182: sole authority — the dock side follows the roamux placement
  // alone; the upstream pref is no longer consulted (it was the source of the
  // "placement does nothing" bug when upstream vertical tabs were on).
  return GetTabStripPlacement(pref_service) == TabStripPlacement::kRight;
}

bool ShouldDockVerticalTabStripLeft(const PrefService* pref_service) {
  // roam-182: see ShouldDockVerticalTabStripRight.
  return GetTabStripPlacement(pref_service) == TabStripPlacement::kLeft;
}

BottomStripLayout ComputeBottomStripLayout(const gfx::Rect& client_area,
                                           int strip_height) {
  const int height = std::clamp(strip_height, 0, client_area.height());
  BottomStripLayout result;
  result.strip = gfx::Rect(client_area.x(), client_area.bottom() - height,
                           client_area.width(), height);
  result.remaining =
      gfx::Rect(client_area.x(), client_area.y(), client_area.width(),
                client_area.height() - height);
  return result;
}

}  // namespace roamux
