// SPDX-License-Identifier: Apache-2.0
// Feature-flag scaffolding proof (roam-3): a Roamex base::Feature toggles on
// demand while the others keep their disabled default (the roam-1 smoke test
// guards all five defaults).

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "roamex/common/roamex_features.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

TEST(RoamexFeaturesTest, FeatureTogglesOnUnderScopedFeatureList) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(roamex::features::kTabVisitNav);
  EXPECT_TRUE(base::FeatureList::IsEnabled(roamex::features::kTabVisitNav));
  // A flag not named in the override keeps its disabled default.
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(roamex::features::kTabStripPosition));
}

} // namespace
