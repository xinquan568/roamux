// SPDX-License-Identifier: Apache-2.0
// roam-254: Tabs.VerticalTabs.* must describe what the user is actually looking
// at — the Roamux effective placement — not the upstream pref that roam-182's
// migration deliberately clears. Before this, a migrated default-on profile
// showing a left-docked vertical strip reported horizontal/disabled.
//
// The contract has two rows, and BOTH are asserted here:
//   kTabStripPosition ON  -> the roamux placement is the sole authority.
//   kTabStripPosition OFF -> the upstream pref stays authoritative, unchanged.
//
// The flag-off fixture must ALSO enable upstream ::tabs::kVerticalTabs,
// otherwise ProvideCurrentSessionData returns early at the
// tabs::IsVerticalTabsFeatureEnabled() gate and emits no sample at all — the
// assertion would then pass vacuously.
//
// Scope: this covers Tabs.VerticalTabs.State, which is reachable through the
// public GetVerticalTabsState()/ProvideCurrentSessionData. The sibling
// histogram Tabs.VerticalTabs.EnabledAtSessionStart is emitted from the private
// OnUserEducationSessionStart, reachable only via the user-education session
// manager; it shares the same helper and is covered by the roamux_unittests
// truth table plus the upstream TabMetricsProviderTest suite (a local-only run
// — no CI job builds unit_tests; see roam-249).

#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/tab_metrics_provider.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/common/tab_strip_placement.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "third_party/metrics_proto/chrome_user_metrics_extension.pb.h"

namespace roamux {
namespace {

constexpr char kStateHistogram[] = "Tabs.VerticalTabs.State";
constexpr char kSessionStartHistogram[] =
    "Tabs.VerticalTabs.EnabledAtSessionStart";

// Drives one ProvideCurrentSessionData pass over the live ProfileManager.
void ProvideSessionData() {
  TabMetricsProvider provider(g_browser_process->profile_manager());
  metrics::ChromeUserMetricsExtension uma_proto;
  provider.ProvideCurrentSessionData(&uma_proto);
}

// Flag ON: the roamux placement owns vertical display (roam-182 sole
// authority), so the telemetry must follow the placement alone.
class RoamuxVerticalTabsTelemetryTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalTabsTelemetryTest() {
    features_.InitWithFeatures({features::kTabStripPosition}, {});
  }

 protected:
  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  base::test::ScopedFeatureList features_;
};

// The reported defect, end to end: a profile that arrived with upstream
// vertical tabs on is migrated to a Left placement with the upstream pref
// cleared, and must report vertical. RED before patch 0062 (the provider read
// the cleared pref and reported kAllHorizontal).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryTest,
                       MigratedProfileReportsVertical) {
  // The post-migration state roam-182 produces: Left placement adopted, the
  // upstream pref cleared. Asserted as state rather than by flipping the
  // upstream pref at runtime and calling MigrateProfilePrefs — doing that
  // inside a live browser drives the display/observer pipeline patch 0008
  // wired into the pref and segfaults. That crash is orthogonal to this
  // issue's telemetry contract and needs its own issue; the state below is
  // exactly what the migration leaves behind, which is what the defect is
  // about.
  SetTabStripPlacement(prefs(), TabStripPlacement::kLeft);
  ASSERT_EQ(TabStripPlacement::kLeft, GetTabStripPlacement(prefs()));
  ASSERT_FALSE(prefs()->GetBoolean(prefs::kUpstreamVerticalTabsEnabled))
      << "the migrated state has the upstream pref cleared";

  base::HistogramTester histograms;
  ProvideSessionData();
  histograms.ExpectUniqueSample(kStateHistogram,
                                VerticalTabsState::kAllVertical, 1);
}

// The UNGATED sibling histogram. TabMetricsProvider's constructor calls
// OnProfileAdded -> OnUserEducationSessionStart for each loaded profile, which
// is the only route to that private method from a test. Without these two cases
// the ungated substitution has NO flag-on regression cover: the upstream
// fixture pins kTabStripPosition off, where the old inline pref read and the
// new helper are observationally identical, so reverting just that one
// substitution would leave every other gate green.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryTest,
                       SessionStartFollowsPlacementNotThePref) {
  SetTabStripPlacement(prefs(), TabStripPlacement::kLeft);
  ASSERT_FALSE(prefs()->GetBoolean(prefs::kUpstreamVerticalTabsEnabled))
      << "the migrated state has the upstream pref cleared";

  base::HistogramTester histograms;
  TabMetricsProvider provider(g_browser_process->profile_manager());
  histograms.ExpectBucketCount(kSessionStartHistogram, true, 1);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryTest,
                       SessionStartReportsFalseForTopPlacementWithStalePref) {
  SetTabStripPlacement(prefs(), TabStripPlacement::kTop);
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);

  base::HistogramTester histograms;
  TabMetricsProvider provider(g_browser_process->profile_manager());
  histograms.ExpectBucketCount(kSessionStartHistogram, false, 1);
}

// A Top placement reports horizontal even if a stale upstream pref says
// otherwise — guards against over-correcting into the inverse defect.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryTest,
                       TopPlacementReportsHorizontalDespiteStaleUpstreamPref) {
  SetTabStripPlacement(prefs(), TabStripPlacement::kTop);
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);

  base::HistogramTester histograms;
  ProvideSessionData();
  histograms.ExpectUniqueSample(kStateHistogram,
                                VerticalTabsState::kAllHorizontal, 1);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryTest,
                       RightPlacementReportsVertical) {
  SetTabStripPlacement(prefs(), TabStripPlacement::kRight);

  base::HistogramTester histograms;
  ProvideSessionData();
  histograms.ExpectUniqueSample(kStateHistogram,
                                VerticalTabsState::kAllVertical, 1);
}

// Flag OFF + upstream vertical tabs ON: the overlay degrades to upstream's
// contract exactly. kVerticalTabs is required here so the capability gate in
// ProvideCurrentSessionData opens and a sample is actually recorded.
class RoamuxVerticalTabsTelemetryFlagOffTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalTabsTelemetryFlagOffTest() {
    features_.InitWithFeatures(/*enabled=*/{::tabs::kVerticalTabs},
                               /*disabled=*/{features::kTabStripPosition});
  }

 protected:
  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryFlagOffTest,
                       FollowsUpstreamPrefWhenEnabled) {
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  // A stored Left placement must be ignored on this path — with the flag off
  // the upstream pref is the authority.
  prefs()->SetInteger(prefs::kTabStripPosition,
                      static_cast<int>(TabStripPlacement::kLeft));

  base::HistogramTester histograms;
  ProvideSessionData();
  histograms.ExpectUniqueSample(kStateHistogram,
                                VerticalTabsState::kAllVertical, 1);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsTelemetryFlagOffTest,
                       FollowsUpstreamPrefWhenDisabled) {
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, false);

  base::HistogramTester histograms;
  ProvideSessionData();
  histograms.ExpectUniqueSample(kStateHistogram,
                                VerticalTabsState::kAllHorizontal, 1);
}

}  // namespace
}  // namespace roamux
