// SPDX-License-Identifier: Apache-2.0
#include "roamux/common/new_tab_position.h"

#include "components/prefs/pref_service.h"
#include "roamux/common/roamux_prefs.h"

namespace roamux {

NewTabPosition GetNewTabPosition(const PrefService* pref_service) {
  if (!pref_service || !pref_service->FindPreference(prefs::kNewTabPosition)) {
    return NewTabPosition::kEndOfActiveGroup;
  }
  const int stored = pref_service->GetInteger(prefs::kNewTabPosition);
  if (stored < static_cast<int>(NewTabPosition::kEndOfStrip) ||
      stored > static_cast<int>(NewTabPosition::kAfterActiveTab)) {
    return NewTabPosition::kEndOfActiveGroup;
  }
  return static_cast<NewTabPosition>(stored);
}

void SetNewTabPosition(PrefService* pref_service, NewTabPosition position) {
  if (!pref_service || !pref_service->FindPreference(prefs::kNewTabPosition)) {
    return;
  }
  pref_service->SetInteger(prefs::kNewTabPosition, static_cast<int>(position));
}

}  // namespace roamux
