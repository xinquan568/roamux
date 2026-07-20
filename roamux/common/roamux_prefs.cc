// SPDX-License-Identifier: Apache-2.0
#include "roamux/common/roamux_prefs.h"

#include "base/feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/tab_strip_placement.h"

namespace roamux::prefs {

namespace {
// Mirror of upstream prefs::kVerticalTabsEnabled — //roamux/common cannot
// depend on //chrome. Local to the migration; roam-182 writes it (the
// "never writes" stance was revoked, maintainer-authorized 2026-07-20).
constexpr char kUpstreamVerticalTabsEnabled[] = "vertical_tabs.enabled";
}  // namespace

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterIntegerPref(kTabStripPosition,
                                0);  // 0 = top (Chromium default)
  registry->RegisterBooleanPref(kReopenClosed,
                                false);  // default: skip closed tabs
  registry->RegisterBooleanPref(kSigninOptionalEntryPoint,
                                false);  // default: sign-in surfaces suppressed
  registry->RegisterDictionaryPref(
      kShortcutBindings);  // §4.3: user rebinds; empty = defaults
}

void MigrateProfilePrefs(PrefService* pref_service) {
  if (!pref_service ||
      !base::FeatureList::IsEnabled(features::kTabStripPosition)) {
    return;
  }
  const PrefService::Preference* upstream =
      pref_service->FindPreference(kUpstreamVerticalTabsEnabled);
  const PrefService::Preference* placement_pref =
      pref_service->FindPreference(kTabStripPosition);
  // Only normalize an explicitly-true, unmanaged upstream pref, and never when
  // EITHER pref is policy-owned — a managed value must not be rewritten and a
  // managed placement must not be forced onto Left. Leave both untouched.
  if (!upstream || upstream->IsManaged() ||
      (placement_pref && placement_pref->IsManaged()) ||
      !pref_service->GetBoolean(kUpstreamVerticalTabsEnabled)) {
    return;
  }
  // "The user asked for vertical": keep an already-vertical roamux placement,
  // else adopt Left (preserving the visible left-docked vertical strip the
  // upstream pref produced). Then clear the upstream pref so the roamux
  // placement is the sole surviving authority.
  const TabStripPlacement placement = GetTabStripPlacement(pref_service);
  if (placement != TabStripPlacement::kLeft &&
      placement != TabStripPlacement::kRight) {
    SetTabStripPlacement(pref_service, TabStripPlacement::kLeft);
  }
  pref_service->SetBoolean(kUpstreamVerticalTabsEnabled, false);
}

}  // namespace roamux::prefs
