// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_COMMON_NEW_TAB_POSITION_H_
#define ROAMUX_COMMON_NEW_TAB_POSITION_H_

class PrefService;

namespace roamux {

// Where an EXPLICITLY created blank tab (Cmd+T, the "+" button, File > New
// Tab, the new-tab-button context menu — every chrome::NewTab() route except
// NewTabTypes::kNoUserAction) is inserted (roam-277). The numeric values are
// the persisted integers of prefs::kNewTabPosition — never renumber.
//
// Names are LOGICAL (end / after); user-facing strings are presentation-aware
// (below / to the right / to the left, RTL) at the UI surface, following
// upstream's IDS_TAB_CXMENU_NEWTABBELOW / NEWTABTORIGHT / NEWTABTOLEFT pattern.
enum class NewTabPosition {
  // End of the tab strip, ungrouped (stock Chromium before roam-275).
  kEndOfStrip = 0,
  // End of the active tab's group; strip end when the active tab is
  // ungrouped. The roam-275 default (upstream kNewTabAddsToActiveGroup,
  // patch 0067), still honouring that upstream flag as an opt-out.
  kEndOfActiveGroup = 1,
  // Directly after the active tab, in its group; after it and ungrouped when
  // the active tab is ungrouped.
  kAfterActiveTab = 2,
};

// The stored position. kEndOfActiveGroup (the registered default) when
// `pref_service` is null, the pref is unregistered (upstream harnesses build
// registries that never ran roamux::prefs::RegisterProfilePrefs — roam-244) or
// the stored value is out of range (prefs are user-editable on disk).
//
// Deliberately NOT gated on roamux::features::kNewTabPosition: chrome::NewTab()
// checks the flag BEFORE calling this, so a disabled flag literally never
// reads the pref (issue design §2).
NewTabPosition GetNewTabPosition(const PrefService* pref_service);

// Persists `position`. No-op when `pref_service` is null or the pref is
// unregistered (PrefService::SetUserPrefValue NOTREACHEDs on an unregistered
// path — same totality as SetTabStripPlacement).
void SetNewTabPosition(PrefService* pref_service, NewTabPosition position);

}  // namespace roamux

#endif  // ROAMUX_COMMON_NEW_TAB_POSITION_H_
