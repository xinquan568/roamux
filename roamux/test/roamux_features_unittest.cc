// SPDX-License-Identifier: Apache-2.0
// Feature-flag scaffolding proof (roam-3): a Roamux base::Feature toggles on
// demand while the others keep their disabled default (the roam-1 smoke test
// guards all five defaults).

#include "roamux/common/roamux_features.h"

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

TEST(RoamuxFeaturesTest, FeatureTogglesOnUnderScopedFeatureList) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(roamux::features::kTabVisitNav);
  EXPECT_TRUE(base::FeatureList::IsEnabled(roamux::features::kTabVisitNav));
  // A flag not named in the override keeps its default. roam-266 graduated
  // kBraveStyleProfiles, so EVERY Roamux flag now ships default-ON and this
  // direction no longer discriminates on its own — see
  // ScopedOverrideLeavesUnnamedFeaturesAtTheirDefault below, which runs the
  // proof in the direction that still can.
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kBraveStyleProfiles));
}

TEST(RoamuxFeaturesTest, ScopedOverrideLeavesUnnamedFeaturesAtTheirDefault) {
  // roam-266: with no disabled-by-default flag left, the scoping proof has to
  // run the other way — DISABLE one flag and require an unnamed one to stay
  // enabled. Without this, a ScopedFeatureList that leaked its override across
  // every Roamux flag would pass the enable-direction test above unnoticed.
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(roamux::features::kTabVisitNav);
  EXPECT_FALSE(base::FeatureList::IsEnabled(roamux::features::kTabVisitNav));
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kBraveStyleProfiles));
}

TEST(RoamuxFeaturesTest, BraveStyleProfilesEnabledByDefault) {
  // roam-266: the E5 Brave-style profiles feature ships enabled by default in
  // v0.0.1-alpha.9 (chrome://flags/#roamux-brave-style-profiles lets users opt
  // out — added by patch 0064 in the same change, since graduating without a
  // kill-switch would have made this the only default-on flag with no
  // off-switch).
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kBraveStyleProfiles));
}

TEST(RoamuxFeaturesTest, TabStripPositionEnabledByDefault) {
  // roam-185: the E1 tab-strip-position feature ships enabled by default (the
  // chrome://flags entry lets users opt out).
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kTabStripPosition));
}

TEST(RoamuxFeaturesTest, InitialUrlEnabledByDefault) {
  // roam-187: the E2 per-tab initial-URL feature ships enabled by default
  // (chrome://flags/#roamux-initial-url lets users opt out).
  EXPECT_TRUE(base::FeatureList::IsEnabled(roamux::features::kInitialUrl));
}

TEST(RoamuxFeaturesTest, TabVisitNavEnabledByDefault) {
  // roam-189: the E4 tab visit-order navigation ships enabled by default
  // (chrome://flags/#roamux-tab-visit-nav lets users opt out).
  EXPECT_TRUE(base::FeatureList::IsEnabled(roamux::features::kTabVisitNav));
}

TEST(RoamuxFeaturesTest, EdgeImportEnabledByDefault) {
  // roam-190: the E3 Microsoft Edge import ships enabled by default
  // (chrome://flags/#roamux-edge-import lets users opt out).
  EXPECT_TRUE(base::FeatureList::IsEnabled(roamux::features::kEdgeImport));
}

TEST(RoamuxFeaturesTest, TabStripToggleShortcutEnabledByDefault) {
  // roam-214: the tab-strip pin/peek toggle ships enabled by default in
  // v0.0.1-alpha.8 (chrome://flags/#roamux-tab-strip-toggle-shortcut lets
  // users opt out).
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kTabStripToggleShortcut));
}

TEST(RoamuxFeaturesTest, BookmarkSubfolderGroupsEnabledByDefault) {
  // roam-208: opening bookmark subfolders as tab groups ships enabled by
  // default in v0.0.1-alpha.8
  // (chrome://flags/#roamux-bookmark-subfolder-groups lets users opt out).
  EXPECT_TRUE(
      base::FeatureList::IsEnabled(roamux::features::kBookmarkSubfolderGroups));
}

TEST(RoamuxFeaturesTest, ExternalOpenProfileEnabledByDefault) {
  // roam-213: the designated external-open profile ships enabled by default in
  // v0.0.1-alpha.8 (chrome://flags/#roamux-external-open-profile lets users
  // opt out). Enabled is inert until a profile is actually designated.
  EXPECT_TRUE(base::FeatureList::IsEnabled(
      roamux::features::kRoamuxExternalOpenProfile));
}

}  // namespace
