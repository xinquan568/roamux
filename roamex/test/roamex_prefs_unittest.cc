// SPDX-License-Identifier: Apache-2.0
// Direct registrar proofs (roam-3): each roamex.* pref registers with its
// documented default and round-trips through a real PrefService. These guard
// registrar semantics; the hook-level proof (upstream registration path) lives
// in roamex_browser_prefs_hook_unittest.cc.

#include "components/prefs/testing_pref_service.h"
#include "roamex/common/roamex_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class RoamexPrefsTest : public testing::Test {
protected:
  RoamexPrefsTest() { roamex::prefs::RegisterProfilePrefs(prefs_.registry()); }

  TestingPrefServiceSimple prefs_;
};

TEST_F(RoamexPrefsTest, AllThreePrefsRegisterWithDefaults) {
  ASSERT_NE(prefs_.FindPreference(roamex::prefs::kTabStripPosition), nullptr);
  ASSERT_NE(prefs_.FindPreference(roamex::prefs::kReopenClosed), nullptr);
  ASSERT_NE(prefs_.FindPreference(roamex::prefs::kSigninOptionalEntryPoint),
            nullptr);
  EXPECT_EQ(prefs_.GetInteger(roamex::prefs::kTabStripPosition), 0); // top
  EXPECT_FALSE(prefs_.GetBoolean(roamex::prefs::kReopenClosed));
  EXPECT_FALSE(prefs_.GetBoolean(roamex::prefs::kSigninOptionalEntryPoint));
}

TEST_F(RoamexPrefsTest, TabStripPositionRoundTrips) {
  prefs_.SetInteger(roamex::prefs::kTabStripPosition, 3); // right
  EXPECT_EQ(prefs_.GetInteger(roamex::prefs::kTabStripPosition), 3);
}

TEST_F(RoamexPrefsTest, ReopenClosedRoundTrips) {
  prefs_.SetBoolean(roamex::prefs::kReopenClosed, true);
  EXPECT_TRUE(prefs_.GetBoolean(roamex::prefs::kReopenClosed));
}

TEST_F(RoamexPrefsTest, SigninOptionalEntryPointRoundTrips) {
  prefs_.SetBoolean(roamex::prefs::kSigninOptionalEntryPoint, true);
  EXPECT_TRUE(prefs_.GetBoolean(roamex::prefs::kSigninOptionalEntryPoint));
}

} // namespace
