// SPDX-License-Identifier: Apache-2.0
// Hook-level proof (roam-3): Chromium's OWN profile-pref registration path
// registers the roamux.* prefs. RED before
// patches/0004-register-profile-prefs.patch is applied; GREEN after. This is
// the test that fails meaningfully in the hook's absence (the direct registrar
// tests cannot).
//
// Fixture mirrors chrome/browser/prefs/browser_prefs_unittest.cc: keyed-service
// registrars CHECK the UI thread (BrowserTaskEnvironment) and need the
// factories built; the Chrome unit-test main
// (//chrome/test:test_support_unit) supplies the TestingBrowserProcess.

#include "base/files/file_path.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.h"
#include "chrome/common/pref_names.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class RoamuxBrowserPrefsHookTest : public testing::Test {
 protected:
  RoamuxBrowserPrefsHookTest() {
    ChromeBrowserMainExtraPartsProfiles::
        EnsureBrowserContextKeyedServiceFactoriesBuilt();
    RegisterUserProfilePrefs(prefs_.registry());
  }

  content::BrowserTaskEnvironment task_environment_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
};

// roam-213 hook-level proof: Chromium's OWN Local State registration path
// (RegisterLocalState — TestingBrowserProcess runs the real one on its
// testing local state) registers the roamux.profiles.* pref pair. RED before
// patches/0049-external-open-local-state-prefs.patch; GREEN after.
TEST_F(RoamuxBrowserPrefsHookTest,
       UpstreamLocalStateRegistrationIncludesRoamuxPrefs) {
  PrefService* local_state = g_browser_process->local_state();
  ASSERT_NE(local_state, nullptr);
  EXPECT_NE(local_state->FindPreference(roamux::prefs::kExternalOpenMode),
            nullptr)
      << "upstream RegisterLocalState did not register "
      << roamux::prefs::kExternalOpenMode
      << " - is patches/0049-external-open-local-state-prefs.patch applied?";
  EXPECT_NE(local_state->FindPreference(roamux::prefs::kExternalOpenProfile),
            nullptr);
  EXPECT_EQ(local_state->GetInteger(roamux::prefs::kExternalOpenMode), 0);
  EXPECT_TRUE(
      local_state->GetString(roamux::prefs::kExternalOpenProfile).empty());
}

TEST_F(RoamuxBrowserPrefsHookTest, UpstreamRegistrationIncludesRoamuxPrefs) {
  EXPECT_NE(prefs_.FindPreference(roamux::prefs::kTabStripPosition), nullptr)
      << "upstream RegisterUserProfilePrefs did not register "
      << roamux::prefs::kTabStripPosition
      << " - is patches/0004-register-profile-prefs.patch applied?";
  EXPECT_NE(prefs_.FindPreference(roamux::prefs::kReopenClosed), nullptr);
  EXPECT_NE(prefs_.FindPreference(roamux::prefs::kSigninOptionalEntryPoint),
            nullptr);
}

// roam-182 hook-level proof: upstream MigrateObsoleteProfilePrefs runs the
// roamux normalization (RED until the 0004 hook line lands).
TEST_F(RoamuxBrowserPrefsHookTest, UpstreamMigrationRunsRoamuxNormalization) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(roamux::features::kTabStripPosition);
  prefs_.SetBoolean(prefs::kVerticalTabsEnabled, true);
  ASSERT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 0);

  MigrateObsoleteProfilePrefs(&prefs_, base::FilePath());

  EXPECT_EQ(prefs_.GetInteger(roamux::prefs::kTabStripPosition), 2)  // left
      << "is the roam-182 migration hook in "
         "patches/0004-register-profile-prefs.patch applied?";
  EXPECT_FALSE(prefs_.GetBoolean(prefs::kVerticalTabsEnabled));
}

}  // namespace
