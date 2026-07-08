// SPDX-License-Identifier: Apache-2.0
// roam-6 (I-1.1): the TabStripPlacement contract — enum<->pref round-trip,
// out-of-range clamping, and flag-off semantics (plan E1; TDD/P6: written RED
// before the implementation).

#include "roamex/common/tab_strip_placement.h"

#include "base/test/scoped_feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "roamex/common/roamex_features.h"
#include "roamex/common/roamex_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamex {
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
}  // namespace roamex
