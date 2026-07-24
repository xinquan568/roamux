// SPDX-License-Identifier: Apache-2.0
// roam-213: the D4 resolution table for external-open profile routing
// (TDD — RED against the S1 nullopt stub). Every row of the table gets a
// test; nullopt always means "current behavior, bit-for-bit".

#include "roamux/browser/profiles/external_open_profile.h"

#include <optional>
#include <string>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/test/scoped_feature_list.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Mirrored literal: upstream switches::kProfileDirectory — //roamux code
// cannot depend on //chrome (precedent: kUpstreamVerticalTabsEnabled in
// roamux_prefs.cc).
constexpr char kProfileDirectorySwitch[] = "profile-directory";

class ExternalOpenProfileTest : public testing::Test {
 protected:
  ExternalOpenProfileTest()
      : command_line_(base::CommandLine::NO_PROGRAM),
        user_data_dir_(FILE_PATH_LITERAL("/tmp/roamux-test-udd")) {
    roamux::prefs::RegisterLocalStatePrefs(local_state_.registry());
    features_.InitAndEnableFeature(
        roamux::features::kRoamuxExternalOpenProfile);
  }

  // The designated-happy-path baseline: flag ON, mode=1, target set,
  // profile exists. Individual tests then break exactly one gate.
  void SetDesignated(const std::string& base_name) {
    local_state_.SetInteger(roamux::prefs::kExternalOpenMode, 1);
    local_state_.SetString(roamux::prefs::kExternalOpenProfile, base_name);
  }

  std::optional<base::FilePath> Resolve(bool exists = true) {
    return roamux::ResolveExternalOpenProfile(
        command_line_, &local_state_, user_data_dir_,
        [exists](const base::FilePath&) { return exists; });
  }

  base::test::ScopedFeatureList features_;
  base::CommandLine command_line_;
  TestingPrefServiceSimple local_state_;
  base::FilePath user_data_dir_;
};

TEST_F(ExternalOpenProfileTest, HappyPathReturnsAppendedPath) {
  SetDesignated("Profile 1");
  const auto path = Resolve();
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(*path, user_data_dir_.Append(FILE_PATH_LITERAL("Profile 1")));
}

TEST_F(ExternalOpenProfileTest, FlagOffReturnsNullopt) {
  // Nested list: overrides the fixture's enable; destroyed before it (LIFO).
  base::test::ScopedFeatureList off;
  off.InitAndDisableFeature(roamux::features::kRoamuxExternalOpenProfile);
  SetDesignated("Profile 1");
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, ModeActiveReturnsNullopt) {
  SetDesignated("Profile 1");
  local_state_.SetInteger(roamux::prefs::kExternalOpenMode, 0);
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, ModeAskReservedReturnsNullopt) {
  // Mode 2 is RESERVED (D3) — until built, it behaves as current behavior.
  SetDesignated("Profile 1");
  local_state_.SetInteger(roamux::prefs::kExternalOpenMode, 2);
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, ProfileDirectorySwitchWinsOverValidPref) {
  SetDesignated("Profile 1");
  command_line_.AppendSwitchASCII(kProfileDirectorySwitch, "Profile 2");
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, EmptyStoredNameReturnsNullopt) {
  SetDesignated("");
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, GuestProfileDirReturnsNullopt) {
  SetDesignated("Guest Profile");
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, SystemProfileDirReturnsNullopt) {
  SetDesignated("System Profile");
  EXPECT_FALSE(Resolve().has_value());
}

TEST_F(ExternalOpenProfileTest, StaleProfileReturnsNullopt) {
  SetDesignated("Profile 9");
  EXPECT_FALSE(Resolve(/*exists=*/false).has_value());
}

TEST_F(ExternalOpenProfileTest, NullLocalStateReturnsNullopt) {
  SetDesignated("Profile 1");
  EXPECT_FALSE(roamux::ResolveExternalOpenProfile(
                   command_line_, nullptr, user_data_dir_,
                   [](const base::FilePath&) { return true; })
                   .has_value());
}

}  // namespace
