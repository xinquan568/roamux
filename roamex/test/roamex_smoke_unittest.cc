// SPDX-License-Identifier: Apache-2.0
#include "base/feature_list.h"
#include "roamex/common/roamex_features.h"
#include "roamex/common/roamex_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// E0 smoke (roam-1 / roam-3): the //roamex overlay links, and every feature flag ships DISABLED by
// default (plan P3 — nothing behavioral is on until its epic completes).
TEST(RoamexSmokeTest, FeatureFlagsDefaultDisabled) {
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamex::features::kTabStripPosition));
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamex::features::kInitialUrl));
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamex::features::kEdgeImport));
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamex::features::kTabVisitNav));
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamex::features::kBraveStyleProfiles));
}

// Pref keys are stable, local, and namespaced under "roamex." (plan §7.2).
TEST(RoamexSmokeTest, PrefKeysAreNamespaced) {
  EXPECT_STREQ("roamex.tabs.strip_position", roamex::prefs::kTabStripPosition);
  EXPECT_STREQ("roamex.signin.optional_entry_point_enabled",
               roamex::prefs::kSigninOptionalEntryPoint);
}

}  // namespace
