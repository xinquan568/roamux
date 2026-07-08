// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_COMMON_TAB_STRIP_PLACEMENT_H_
#define ROAMEX_COMMON_TAB_STRIP_PLACEMENT_H_

class PrefService;

namespace roamex {

// Where the tab strip is docked (E1, flag roamex::features::kTabStripPosition).
// The numeric values are the persisted integers of prefs::kTabStripPosition —
// the forward contract I-1.2/I-1.3/I-1.4 consume; never renumber (stored user
// state depends on it).
enum class TabStripPlacement {
  kTop = 0,  // Chromium default (the registered pref default).
  kBottom = 1,
  kLeft = 2,
  kRight = 3,
};

// The effective placement. kTop when the feature flag is off, `pref_service` is
// null, or the stored value is out of range (prefs are user-editable on disk) —
// consumers can never half-honor a placement while the epic flag is off.
TabStripPlacement GetTabStripPlacement(const PrefService* pref_service);

// Persists `placement`. No-op when `pref_service` is null.
void SetTabStripPlacement(PrefService* pref_service,
                          TabStripPlacement placement);

}  // namespace roamex

#endif  // ROAMEX_COMMON_TAB_STRIP_PLACEMENT_H_
