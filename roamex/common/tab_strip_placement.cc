// SPDX-License-Identifier: Apache-2.0
#include "roamex/common/tab_strip_placement.h"

#include "base/feature_list.h"
#include "components/prefs/pref_service.h"
#include "roamex/common/roamex_features.h"
#include "roamex/common/roamex_prefs.h"

namespace roamex {

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

}  // namespace roamex
