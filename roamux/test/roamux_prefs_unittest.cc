// SPDX-License-Identifier: Apache-2.0
// Direct registrar proofs (roam-3): each roamux.* pref registers with its
// documented default and round-trips through a real PrefService. These guard
// registrar semantics; the hook-level proof (upstream registration path) lives
// in roamux_browser_prefs_hook_unittest.cc.

#include "roamux/common/roamux_prefs.h"

#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "roamux/common/roamux_features.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class RoamuxPrefsTest : public testing::Test {
 protected:
  RoamuxPrefsTest() { roamux::prefs::RegisterProfilePrefs(prefs_.registry()); }

  TestingPrefServiceSimple prefs_;
};

TEST_F(RoamuxPrefsTest, AllThreePrefsRegisterWithDefaults) {
  ASSERT_NE(prefs_.FindPreference(roamux::prefs::kTabStripPosition), nullptr);
  ASSERT_NE(prefs_.FindPreference(roamux::prefs::kReopenClosed), nullptr);
  ASSERT_NE(prefs_.FindPreference(roamux::prefs::kSigninOptionalEntryPoint),
            nullptr);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 0);  // top
  EXPECT_FALSE(prefs_.GetBoolean(roamux::prefs::kReopenClosed));
  EXPECT_FALSE(prefs_.GetBoolean(roamux::prefs::kSigninOptionalEntryPoint));
}

TEST_F(RoamuxPrefsTest, TabStripPositionRoundTrips) {
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 3);  // right
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 3);
}

TEST_F(RoamuxPrefsTest, ReopenClosedRoundTrips) {
  prefs_.SetBoolean(roamux::prefs::kReopenClosed, true);
  EXPECT_TRUE(prefs_.GetBoolean(roamux::prefs::kReopenClosed));
}

TEST_F(RoamuxPrefsTest, SigninOptionalEntryPointRoundTrips) {
  prefs_.SetBoolean(roamux::prefs::kSigninOptionalEntryPoint, true);
  EXPECT_TRUE(prefs_.GetBoolean(roamux::prefs::kSigninOptionalEntryPoint));
}

// roam-213: Local State prefs for external-open routing (TDD — RED against
// the S1 no-op registrar stub). Browser-global by design (D3): they select
// BETWEEN profiles, so they cannot live in profile prefs.
class RoamuxLocalStatePrefsTest : public testing::Test {
 protected:
  RoamuxLocalStatePrefsTest() {
    roamux::prefs::RegisterLocalStatePrefs(prefs_.registry());
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(RoamuxLocalStatePrefsTest, BothPrefsRegisterWithDefaults) {
  ASSERT_NE(prefs_.FindPreference(roamux::prefs::kExternalOpenMode), nullptr);
  ASSERT_NE(prefs_.FindPreference(roamux::prefs::kExternalOpenProfile),
            nullptr);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kExternalOpenMode),
            0);  // active profile — current behavior
  EXPECT_TRUE(prefs_.GetString(roamux::prefs::kExternalOpenProfile).empty());
}

TEST_F(RoamuxLocalStatePrefsTest, ModeRoundTrips) {
  prefs_.SetInteger(roamux::prefs::kExternalOpenMode, 1);  // designated
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kExternalOpenMode), 1);
}

TEST_F(RoamuxLocalStatePrefsTest, ProfileBaseNameRoundTrips) {
  prefs_.SetString(roamux::prefs::kExternalOpenProfile, "Profile 1");
  EXPECT_EQ(prefs_.GetString(roamux::prefs::kExternalOpenProfile),
            "Profile 1");
}

// roam-182: startup normalization of the upstream vertical-tabs pref onto the
// roamux placement (sole-authority contract; TDD — RED against the S1 no-op
// stub). Mirrored literal: the upstream pref is registered by //chrome, which
// this unit target does not link.
inline constexpr char kUpstreamVerticalTabsPref[] = "vertical_tabs.enabled";

class RoamuxPrefsMigrationTest : public testing::Test {
 protected:
  RoamuxPrefsMigrationTest() {
    roamux::prefs::RegisterProfilePrefs(prefs_.registry());
    prefs_.registry()->RegisterBooleanPref(kUpstreamVerticalTabsPref, false);
    features_.InitAndEnableFeature(roamux::features::kTabStripPosition);
  }

  base::test::ScopedFeatureList features_;
  TestingPrefServiceSimple prefs_;
};

TEST_F(RoamuxPrefsMigrationTest, UpstreamOnDefaultPlacementBecomesLeft) {
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 2);  // left
  EXPECT_FALSE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST_F(RoamuxPrefsMigrationTest, UpstreamOnKeepsStoredRightPlacement) {
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 3);  // right
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 3);
  EXPECT_FALSE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST_F(RoamuxPrefsMigrationTest, UpstreamOffTouchesNothing) {
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 1);  // bottom
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 1);
  EXPECT_FALSE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST_F(RoamuxPrefsMigrationTest, LaterTopChoiceNeverReMigrates) {
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  // The user later picks Top via the roamux control (upstream stays false).
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 0);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 0);
}

TEST_F(RoamuxPrefsMigrationTest, ExplicitReEnableNormalizesAgain) {
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 0);  // top
  // Post-migration direct write (the still-visible upstream Appearance row
  // binds the pref directly): honored as "user asked for vertical" at the
  // next startup — documented normalization, not a one-time sentinel.
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 2);  // left
  EXPECT_FALSE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST_F(RoamuxPrefsMigrationTest, ManagedUpstreamPrefIsUntouched) {
  prefs_.SetManagedPref(kUpstreamVerticalTabsPref, base::Value(true));
  prefs_.SetInteger(roamux::prefs::kTabStripPosition, 0);
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 0);
  EXPECT_TRUE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST_F(RoamuxPrefsMigrationTest, ManagedPlacementBlocksMigration) {
  // A policy-owned placement must not be forced onto Left, and the upstream
  // pref must not be cleared underneath it.
  prefs_.SetBoolean(kUpstreamVerticalTabsPref, true);
  prefs_.SetManagedPref(roamux::prefs::kTabStripPosition, base::Value(0));
  roamux::prefs::MigrateProfilePrefs(&prefs_);
  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 0);
  EXPECT_TRUE(prefs_.GetBoolean(kUpstreamVerticalTabsPref));
}

TEST(RoamuxPrefsMigrationFlagOffTest, FlagOffIsStrictNoOp) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(roamux::features::kTabStripPosition);
  TestingPrefServiceSimple prefs;
  roamux::prefs::RegisterProfilePrefs(prefs.registry());
  prefs.registry()->RegisterBooleanPref(kUpstreamVerticalTabsPref, false);
  prefs.SetBoolean(kUpstreamVerticalTabsPref, true);
  roamux::prefs::MigrateProfilePrefs(&prefs);
  EXPECT_EQ(prefs.GetInteger(roamux::prefs::kTabStripPosition), 0);
  EXPECT_TRUE(prefs.GetBoolean(kUpstreamVerticalTabsPref));
}

}  // namespace
