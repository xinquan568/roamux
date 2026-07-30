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
  // roam-244: stay total over pref services whose registry never ran
  // roamux::prefs::RegisterProfilePrefs. Upstream test harnesses legitimately
  // build a PrefService carrying only the upstream tab prefs and then construct
  // objects that reach here — patch 0008 wires this call into
  // VerticalTabStripStateController's constructor — and GetInteger CHECKs on an
  // unregistered pref, taking the whole harness down in SetUp. Production
  // registration is guaranteed by patch 0004 and independently asserted by
  // RoamuxBrowserPrefsHookTest.UpstreamRegistrationIncludesRoamuxPrefs, so this
  // fallback cannot mask a shipped regression.
  if (!pref_service->FindPreference(prefs::kTabStripPosition)) {
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
  // roam-244: the write side needs the same totality as the read side —
  // PrefService::SetUserPrefValue rejects an unregistered path (NOTREACHED),
  // so on such a registry the placement is simply unwritable and the store is
  // skipped rather than crashing the caller.
  if (!pref_service || !pref_service->FindPreference(prefs::kTabStripPosition)) {
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

bool IsVerticalTabStripOnLogicalTrailingEdge(const PrefService* pref_service,
                                             bool is_rtl) {
  // roam-228: physical placement XOR RTL. When the roamux placement does not
  // drive a vertical strip there is no roamux dock side to translate, so the
  // answer is false and consumers keep upstream's logical-leading behaviour.
  const bool physical_right = ShouldDockVerticalTabStripRight(pref_service);
  const bool physical_left = ShouldDockVerticalTabStripLeft(pref_service);
  if (!physical_right && !physical_left) {
    return false;
  }
  return physical_right != is_rtl;
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
