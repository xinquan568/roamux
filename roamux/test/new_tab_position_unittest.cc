// SPDX-License-Identifier: Apache-2.0
// roam-277: the roamux.tabs.new_tab_position accessor — total over null /
// unregistered / out-of-range (the roam-244 totality idiom of
// GetTabStripPlacement) and flag-AGNOSTIC: chrome::NewTab() owns the
// kNewTabPosition check so that flag-off never reads the pref (TDD/P6:
// written RED before the implementation).

#include "roamux/common/new_tab_position.h"

#include "base/test/scoped_feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux {
namespace {

class NewTabPositionTest : public testing::Test {
 protected:
  NewTabPositionTest() { prefs::RegisterProfilePrefs(prefs_.registry()); }

  TestingPrefServiceSimple prefs_;
};

TEST_F(NewTabPositionTest, NullServiceReadsDefault) {
  EXPECT_EQ(NewTabPosition::kEndOfActiveGroup, GetNewTabPosition(nullptr));
}

TEST_F(NewTabPositionTest, UnregisteredRegistryReadsDefaultAndSetIsNoOp) {
  TestingPrefServiceSimple bare;  // RegisterProfilePrefs never ran.
  EXPECT_EQ(NewTabPosition::kEndOfActiveGroup, GetNewTabPosition(&bare));
  SetNewTabPosition(&bare, NewTabPosition::kAfterActiveTab);  // must not CHECK
  EXPECT_EQ(nullptr, bare.FindPreference(prefs::kNewTabPosition));
}

TEST_F(NewTabPositionTest, RegisteredDefaultIsEndOfActiveGroup) {
  EXPECT_EQ(1, prefs_.GetInteger(prefs::kNewTabPosition));
  EXPECT_EQ(NewTabPosition::kEndOfActiveGroup, GetNewTabPosition(&prefs_));
}

TEST_F(NewTabPositionTest, StoredValuesRoundTrip) {
  for (const NewTabPosition mode :
       {NewTabPosition::kEndOfStrip, NewTabPosition::kEndOfActiveGroup,
        NewTabPosition::kAfterActiveTab}) {
    SetNewTabPosition(&prefs_, mode);
    EXPECT_EQ(static_cast<int>(mode),
              prefs_.GetInteger(prefs::kNewTabPosition));
    EXPECT_EQ(mode, GetNewTabPosition(&prefs_));
  }
}

TEST_F(NewTabPositionTest, OutOfRangeStoredValueReadsDefault) {
  for (const int garbage : {-1, 3, 99}) {
    prefs_.SetInteger(prefs::kNewTabPosition, garbage);
    EXPECT_EQ(NewTabPosition::kEndOfActiveGroup, GetNewTabPosition(&prefs_))
        << garbage;
  }
}

// The accessor is deliberately NOT gated on kNewTabPosition: the seam is.
TEST_F(NewTabPositionTest, AccessorIgnoresTheFeatureFlag) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(features::kNewTabPosition);
  SetNewTabPosition(&prefs_, NewTabPosition::kAfterActiveTab);
  EXPECT_EQ(NewTabPosition::kAfterActiveTab, GetNewTabPosition(&prefs_));
}

}  // namespace
}  // namespace roamux
