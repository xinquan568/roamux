// SPDX-License-Identifier: Apache-2.0
// roam-6 (I-1.1): the TabStripPlacement contract — enum<->pref round-trip,
// out-of-range clamping, and flag-off semantics (plan E1; TDD/P6: written RED
// before the implementation).

#include "roamux/common/tab_strip_placement.h"

#include "base/test/scoped_feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux {
namespace {

class TabStripPlacementTest : public testing::Test {
 protected:
  TabStripPlacementTest() {
    prefs::RegisterProfilePrefs(pref_service_.registry());
  }

  TestingPrefServiceSimple pref_service_;
};

TEST_F(TabStripPlacementTest, DefaultIsTop) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(&pref_service_));
}

TEST_F(TabStripPlacementTest, RoundTripsAllPlacements) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  for (TabStripPlacement placement :
       {TabStripPlacement::kTop, TabStripPlacement::kBottom,
        TabStripPlacement::kLeft, TabStripPlacement::kRight}) {
    SetTabStripPlacement(&pref_service_, placement);
    EXPECT_EQ(placement, GetTabStripPlacement(&pref_service_));
  }
}

TEST_F(TabStripPlacementTest, PersistsAsTheRegisteredIntegerPref) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_EQ(3, pref_service_.GetInteger(prefs::kTabStripPosition));
}

TEST_F(TabStripPlacementTest, ClampsOutOfRangeStoredValuesToTop) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  for (int garbage : {-1, 4, 99}) {
    pref_service_.SetInteger(prefs::kTabStripPosition, garbage);
    EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(&pref_service_))
        << "stored value " << garbage;
  }
}

TEST_F(TabStripPlacementTest, FlagOffAlwaysReadsTop) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(features::kTabStripPosition);
  pref_service_.SetInteger(prefs::kTabStripPosition, 2);  // kLeft persisted.
  EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(&pref_service_));
}

TEST_F(TabStripPlacementTest, NullPrefServiceReadsTop) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(nullptr));
}

}  // namespace
}  // namespace roamux

// roam-7 (I-1.2): the bottom-band geometry helper (TDD: RED before the
// implementation).
#include "ui/gfx/geometry/rect.h"

namespace roamux {
namespace {

TEST(ComputeBottomStripLayoutTest, CarvesBottomBandOfStripHeight) {
  const gfx::Rect client(0, 0, 1200, 800);
  const BottomStripLayout result = ComputeBottomStripLayout(client, 40);
  EXPECT_EQ(gfx::Rect(0, 760, 1200, 40), result.strip);
  EXPECT_EQ(gfx::Rect(0, 0, 1200, 760), result.remaining);
}

TEST(ComputeBottomStripLayoutTest, HonorsClientOrigin) {
  const gfx::Rect client(10, 20, 600, 400);
  const BottomStripLayout result = ComputeBottomStripLayout(client, 50);
  EXPECT_EQ(gfx::Rect(10, 370, 600, 50), result.strip);
  EXPECT_EQ(gfx::Rect(10, 20, 600, 350), result.remaining);
}

TEST(ComputeBottomStripLayoutTest, ClampsOversizedStripToClientHeight) {
  const gfx::Rect client(0, 0, 300, 30);
  const BottomStripLayout result = ComputeBottomStripLayout(client, 50);
  EXPECT_EQ(gfx::Rect(0, 0, 300, 30), result.strip);
  EXPECT_EQ(gfx::Rect(0, 0, 300, 0), result.remaining);
}

TEST(ComputeBottomStripLayoutTest, NegativeStripHeightYieldsEmptyBand) {
  const gfx::Rect client(0, 0, 300, 200);
  const BottomStripLayout result = ComputeBottomStripLayout(client, -5);
  EXPECT_EQ(gfx::Rect(0, 200, 300, 0), result.strip);
  EXPECT_EQ(client, result.remaining);
}

}  // namespace
}  // namespace roamux

// roam-8 (I-1.3): vertical-display + right-dock predicates (TDD: RED first).
namespace roamux {
namespace {

// roam-182: the upstream pref is registered here only to prove the dock
// predicates IGNORE it (sole-authority contract). Local literal — the mirror
// constant retired from the predicate API with roam-182.
inline constexpr char kUpstreamVerticalTabsPref[] = "vertical_tabs.enabled";

class VerticalPlacementPredicateTest : public testing::Test {
 protected:
  VerticalPlacementPredicateTest() {
    prefs::RegisterProfilePrefs(pref_service_.registry());
    pref_service_.registry()->RegisterBooleanPref(kUpstreamVerticalTabsPref,
                                                  false);
  }

  TestingPrefServiceSimple pref_service_;
};

TEST_F(VerticalPlacementPredicateTest, DisplayTruthTable) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_FALSE(ShouldDisplayVerticalTabsForPlacement(nullptr));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kTop);
  EXPECT_FALSE(ShouldDisplayVerticalTabsForPlacement(&pref_service_));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kBottom);
  EXPECT_FALSE(ShouldDisplayVerticalTabsForPlacement(&pref_service_));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  EXPECT_TRUE(ShouldDisplayVerticalTabsForPlacement(&pref_service_));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_TRUE(ShouldDisplayVerticalTabsForPlacement(&pref_service_));
}

TEST_F(VerticalPlacementPredicateTest, DisplayFalseWhenFlagOff) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  EXPECT_FALSE(ShouldDisplayVerticalTabsForPlacement(&pref_service_));
}

TEST_F(VerticalPlacementPredicateTest, RightDockFollowsPlacementAlone) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_TRUE(ShouldDockVerticalTabStripRight(&pref_service_));
  // roam-182 sole authority: the upstream pref no longer moves the dock.
  pref_service_.SetBoolean(kUpstreamVerticalTabsPref, true);
  EXPECT_TRUE(ShouldDockVerticalTabStripRight(&pref_service_));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  EXPECT_FALSE(ShouldDockVerticalTabStripRight(&pref_service_));
}

TEST_F(VerticalPlacementPredicateTest, LeftDockFollowsPlacementAlone) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  EXPECT_TRUE(ShouldDockVerticalTabStripLeft(&pref_service_));
  pref_service_.SetBoolean(kUpstreamVerticalTabsPref, true);
  EXPECT_TRUE(ShouldDockVerticalTabStripLeft(&pref_service_));
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_FALSE(ShouldDockVerticalTabStripLeft(&pref_service_));
}

TEST_F(VerticalPlacementPredicateTest,
       RightDockToleratesUnregisteredUpstreamPref) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  TestingPrefServiceSimple bare;
  prefs::RegisterProfilePrefs(bare.registry());  // upstream pref NOT registered
  SetTabStripPlacement(&bare, TabStripPlacement::kRight);
  EXPECT_TRUE(ShouldDockVerticalTabStripRight(&bare));
}

// roam-228: the LOGICAL dock side = physical placement XOR RTL. The physical
// predicates above are RTL-invariant by contract (roam-9 D1); this one is the
// deliberate translation for views' logical coordinate space and for
// views::ResizeArea's already-RTL-normalised delta. (TDD: RED first.)
TEST_F(VerticalPlacementPredicateTest, LogicalTrailingEdgeTruthTable) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);

  // kTop/kBottom never drive a vertical strip: no logical dock side at all.
  for (const auto placement :
       {TabStripPlacement::kTop, TabStripPlacement::kBottom}) {
    SetTabStripPlacement(&pref_service_, placement);
    EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                         /*is_rtl=*/false));
    EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                         /*is_rtl=*/true));
  }

  // Physical LEFT: logical leading in LTR, logical TRAILING in RTL.
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                       /*is_rtl=*/false));
  EXPECT_TRUE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                      /*is_rtl=*/true));

  // Physical RIGHT: logical TRAILING in LTR, logical leading in RTL.
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_TRUE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                      /*is_rtl=*/false));
  EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                       /*is_rtl=*/true));
}

TEST_F(VerticalPlacementPredicateTest, LogicalTrailingEdgeFalseWhenFlagOff) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(features::kTabStripPosition);
  for (const auto placement :
       {TabStripPlacement::kTop, TabStripPlacement::kBottom,
        TabStripPlacement::kLeft, TabStripPlacement::kRight}) {
    SetTabStripPlacement(&pref_service_, placement);
    EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                         /*is_rtl=*/false));
    EXPECT_FALSE(IsVerticalTabStripOnLogicalTrailingEdge(&pref_service_,
                                                         /*is_rtl=*/true));
  }
}

TEST_F(VerticalPlacementPredicateTest,
       LogicalTrailingEdgeNullPrefServiceIsFalse) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_FALSE(
      IsVerticalTabStripOnLogicalTrailingEdge(nullptr, /*is_rtl=*/false));
  EXPECT_FALSE(
      IsVerticalTabStripOnLogicalTrailingEdge(nullptr, /*is_rtl=*/true));
}

// roam-244: the accessors must be TOTAL over pref services whose registry never
// ran roamux::prefs::RegisterProfilePrefs. Upstream test harnesses legitimately
// build a PrefService with only the upstream tab prefs registered and then
// construct objects that reach these accessors (patch 0008 makes
// VerticalTabStripStateController's constructor do exactly that), so a bare
// GetInteger CHECK-crashes the harness in SetUp and a bare SetInteger hits
// PrefService::SetUserPrefValue's unregistered path. Production is unaffected:
// registration is guaranteed by patch 0004 and independently asserted by
// RoamuxBrowserPrefsHookTest.UpstreamRegistrationIncludesRoamuxPrefs.
// TDD/P6: both cases were written and RED-run before the accessors were made
// total (the getter crashed, the setter tripped the unregistered-write path).
class TabStripPlacementUnregisteredPrefTest : public testing::Test {
 protected:
  // Deliberately NOT registered — this is the harness shape under test.
  TestingPrefServiceSimple pref_service_;
};

TEST_F(TabStripPlacementUnregisteredPrefTest,
       GetFallsBackToTopWithoutCrashing) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(&pref_service_));
}

TEST_F(TabStripPlacementUnregisteredPrefTest, SetIsANoOpWithoutCrashing) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  // The write cannot land anywhere, so the read still reports the fallback.
  EXPECT_EQ(TabStripPlacement::kTop, GetTabStripPlacement(&pref_service_));
}

// ---------------------------------------------------------------------------
// roam-254: IsVerticalTabStripEffectivelyEnabled — the authority split that
// Tabs.VerticalTabs.* telemetry must express. Written RED before the
// implementation (TDD/P6). The contract has exactly two rows:
//   flag ON  -> the roamux placement alone (roam-182 sole authority)
//   flag OFF -> the upstream pref alone, read exactly as upstream reads it
// ---------------------------------------------------------------------------

class EffectiveVerticalTabsTest : public testing::Test {
 protected:
  EffectiveVerticalTabsTest() {
    prefs::RegisterProfilePrefs(pref_service_.registry());
    // The upstream mirror pref is registered by //chrome, which //roamux/common
    // cannot depend on; register it here so the flag-off branch has a real
    // pref to read.
    pref_service_.registry()->RegisterBooleanPref(
        prefs::kUpstreamVerticalTabsEnabled, false);
  }

  TestingPrefServiceSimple pref_service_;
};

TEST_F(EffectiveVerticalTabsTest,
       FlagOnLeftPlacementIsVerticalDespiteClearedPref) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kLeft);
  // Exactly the migrated-profile shape roam-182 produces: placement adopted,
  // upstream pref cleared. This is the reported defect.
  pref_service_.SetBoolean(prefs::kUpstreamVerticalTabsEnabled, false);
  EXPECT_TRUE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

TEST_F(EffectiveVerticalTabsTest, FlagOnRightPlacementIsVertical) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kRight);
  EXPECT_TRUE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

TEST_F(EffectiveVerticalTabsTest, FlagOnTopPlacementIgnoresATrueUpstreamPref) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kTop);
  pref_service_.SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  // Guards against over-correcting: with the flag on the placement is the SOLE
  // authority, so a stale true upstream pref must not resurrect vertical.
  EXPECT_FALSE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

TEST_F(EffectiveVerticalTabsTest, FlagOnBottomPlacementIsNotVertical) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  SetTabStripPlacement(&pref_service_, TabStripPlacement::kBottom);
  EXPECT_FALSE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

TEST_F(EffectiveVerticalTabsTest, FlagOffFollowsTheUpstreamPref) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(features::kTabStripPosition);
  // A Left placement is stored but must be ignored: with the flag off the
  // upstream pref is authoritative, byte-identical to upstream today.
  pref_service_.SetInteger(prefs::kTabStripPosition,
                           static_cast<int>(TabStripPlacement::kLeft));
  pref_service_.SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  EXPECT_TRUE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));

  pref_service_.SetBoolean(prefs::kUpstreamVerticalTabsEnabled, false);
  EXPECT_FALSE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

TEST_F(EffectiveVerticalTabsTest, NullPrefServiceIsNotVertical) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  EXPECT_FALSE(IsVerticalTabStripEffectivelyEnabled(nullptr));
}

// roam-244 totality, scoped to the placement branch: a registry that never ran
// roamux::prefs::RegisterProfilePrefs must not crash. (The flag-OFF branch
// deliberately keeps upstream's CHECK on an unregistered upstream pref, so
// there is no matching case for it — preserving that crash is the point.)
class EffectiveVerticalTabsUnregisteredPlacementTest : public testing::Test {
 protected:
  EffectiveVerticalTabsUnregisteredPlacementTest() {
    pref_service_.registry()->RegisterBooleanPref(
        prefs::kUpstreamVerticalTabsEnabled, false);
  }

  // roamux placement pref deliberately NOT registered.
  TestingPrefServiceSimple pref_service_;
};

TEST_F(EffectiveVerticalTabsUnregisteredPlacementTest,
       FlagOnFallsBackToNotVerticalWithoutCrashing) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(features::kTabStripPosition);
  pref_service_.SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  EXPECT_FALSE(IsVerticalTabStripEffectivelyEnabled(&pref_service_));
}

}  // namespace
}  // namespace roamux
