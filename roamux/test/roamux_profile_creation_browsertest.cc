// SPDX-License-Identifier: Apache-2.0
// roam-29 (I-5.1) route-level contract: with kBraveStyleProfiles on, the
// picker's NEW_PROFILE route computes to localProfileCustomization — the
// sign-in/type-choice step never exists — and the app's automatic
// continueWithoutAccount path creates a local profile with no account/token
// and its own directory/prefs. Flag-off: pure pass-through (type-choice
// preserved where Dice allows). TDD/P6: written RED before patch 0022.

#include <string>

#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/feedback/feedback_uploader_factory_chrome.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/signin/account_consistency_mode_manager.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/profiles/profile_picker.h"
#include "chrome/browser/ui/views/profiles/profile_picker_test_base.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/search_engines_switches.h"
#include "components/signin/public/base/consent_level.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// Clicks the main view's Add-profile button (the user's in-page navigation to
// the NEW_PROFILE route) and atomically reads the route state pushed by the
// picker's navigation mixin plus whether the sign-in/type-choice view is
// active. history.pushState runs synchronously inside click(), so the read
// cannot race the C++ side closing the picker.
constexpr char kClickAddProfileAndReadState[] = R"(
    (() => {
      const app = document.querySelector('profile-picker-app');
      const main = app.shadowRoot.querySelector('profile-picker-main-view');
      const add = main.shadowRoot.querySelector('#addProfile');
      add.click();
      const typeChoice = app.shadowRoot.querySelector('profile-type-choice');
      return JSON.stringify({
        step: history.state.step,
        typeChoiceActive:
            !!(typeChoice && typeChoice.classList.contains('active')),
      });
    })();
)";

class RoamuxProfileCreationBrowserTestBase : public ProfilePickerTestBase {
 public:
  RoamuxProfileCreationBrowserTestBase() {
    // roam-99: foreign hierarchy — cannot re-base onto RoamuxBrowserTest.
    roamux::test::DisableWebUIToolbarFeatures(webui_toolbar_disables_);
  }

  // roam-223: foreign hierarchy — cannot inherit RoamuxBrowserTest's
  // suppression, so install it here. Dispatched immediately before each
  // profile's keyed services are created, so it covers profiles the picker
  // creates mid-test, not just the startup profile.
  void SetUpBrowserContextKeyedServices(
      content::BrowserContext* context) override {
    ProfilePickerTestBase::SetUpBrowserContextKeyedServices(context);
    roamux::test::SuppressFeedbackUploaderForTesting(context);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    ProfilePickerTestBase::SetUpCommandLine(command_line);
    // Keep the signed-out post-identity flow deterministic in the harness —
    // the search-engine-choice step must not interpose before FinishFlow.
    command_line->AppendSwitch(switches::kDisableSearchEngineChoiceScreen);
  }

  // Opens the picker on the main view and waits for the WebUI to load.
  void OpenPickerMainView() {
    ProfilePicker::Show(ProfilePicker::Params::FromEntryPoint(
        ProfilePicker::EntryPoint::kProfileMenuManageProfiles));
    WaitForPickerWidgetCreated();
    WaitForLoadStop(GURL("chrome://profile-picker"));
  }

  // Returns {step, typeChoiceActive} observed synchronously after the click.
  std::pair<std::string, bool> ClickAddProfileAndReadState() {
    const std::string json =
        content::EvalJs(web_contents(), kClickAddProfileAndReadState)
            .ExtractString();
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    CHECK(dict.has_value());
    return {*dict->FindString("step"),
            dict->FindBool("typeChoiceActive").value_or(true)};
  }

 private:
  base::test::ScopedFeatureList webui_toolbar_disables_;
};

class RoamuxProfileCreationFlagOnBrowserTest
    : public RoamuxProfileCreationBrowserTestBase {
 public:
  RoamuxProfileCreationFlagOnBrowserTest() {
    features_.InitAndEnableFeature(roamux::features::kBraveStyleProfiles);
  }

 private:
  base::test::ScopedFeatureList features_;
};

class RoamuxProfileCreationFlagOffBrowserTest
    : public RoamuxProfileCreationBrowserTestBase {
 public:
  RoamuxProfileCreationFlagOffBrowserTest() {
    features_.InitAndDisableFeature(roamux::features::kBraveStyleProfiles);
  }

 private:
  base::test::ScopedFeatureList features_;
};

// Route-level + identity + isolation contract (issue Tests, all three points).
IN_PROC_BROWSER_TEST_F(RoamuxProfileCreationFlagOnBrowserTest,
                       CreationIsNameOnlyNoAccountAndIsolated) {
  Profile* original_profile = browser()->profile();
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  const size_t initial_profiles = profile_manager->GetNumberOfProfiles();

  OpenPickerMainView();

  // Entering profile creation resolves straight to the local name-only step
  // (the picker WebUI computes it from the suppressed
  // signInProfileCreationFlowSupported boolean, whose C++ seam is pinned by
  // BraveStyleProfilesTest); the sign-in/type-choice view is never active.
  auto [step, type_choice_active] = ClickAddProfileAndReadState();
  EXPECT_EQ(step, "localProfileCustomization");
  EXPECT_FALSE(type_choice_active);

  // The app's automatic continueWithoutAccount path creates the profile and
  // opens its browser (naming/theme continue in the customization surface).
  Browser* new_browser = ui_test_utils::WaitForBrowserToOpen();
  ASSERT_NE(new_browser, nullptr);
  Profile* new_profile = new_browser->profile();
  ASSERT_NE(new_profile, nullptr);
  EXPECT_EQ(profile_manager->GetNumberOfProfiles(), initial_profiles + 1);

  // No account or token state is created.
  signin::IdentityManager* identity =
      IdentityManagerFactory::GetForProfile(new_profile);
  ASSERT_NE(identity, nullptr);
  EXPECT_TRUE(identity->GetAccountsWithRefreshTokens().empty());
  EXPECT_FALSE(identity->HasPrimaryAccount(signin::ConsentLevel::kSignin));

  // Immediately isolated: own directory and own prefs.
  EXPECT_NE(new_profile, original_profile);
  EXPECT_NE(new_profile->GetPath(), original_profile->GetPath());
  EXPECT_NE(new_profile->GetPrefs(), original_profile->GetPrefs());
}

// roam-223 env-guard (TDD: born RED — see the RED commit). These fixtures sit
// on the foreign ProfilePickerTestBase hierarchy and so cannot inherit
// RoamuxBrowserTest's feedback-uploader suppression; they call
// SuppressFeedbackUploaderForTesting() from their own
// SetUpBrowserContextKeyedServices override instead. This guard is what proves
// that override is present and effective — the shared-base guard in
// roamux_test_env_browsertest.cc structurally cannot see this path.
//
// It asserts on a *newly created* profile as well as the initial one, because
// the suppression is installed per-BrowserContext by a hook dispatched at each
// profile's keyed-service creation: a gate that covered only the startup
// profile would still leak a BEST_EFFORT + BLOCK_SHUTDOWN task for every
// profile a test creates.
IN_PROC_BROWSER_TEST_F(RoamuxProfileCreationFlagOnBrowserTest,
                       FeedbackUploaderIsNotInstantiatedForCreatedProfiles) {
  Profile* original_profile = browser()->profile();
  EXPECT_EQ(nullptr,
            feedback::FeedbackUploaderFactoryChrome::GetForBrowserContext(
                original_profile));

  OpenPickerMainView();
  auto [step, type_choice_active] = ClickAddProfileAndReadState();
  EXPECT_EQ(step, "localProfileCustomization");

  Browser* new_browser = ui_test_utils::WaitForBrowserToOpen();
  ASSERT_NE(new_browser, nullptr);
  Profile* new_profile = new_browser->profile();
  ASSERT_NE(new_profile, nullptr);
  ASSERT_NE(new_profile, original_profile);

  EXPECT_EQ(nullptr,
            feedback::FeedbackUploaderFactoryChrome::GetForBrowserContext(
                new_profile));
}

// Flag-off is a pure pass-through: the boolean equals upstream's Dice value
// and the type-choice step is preserved (where Dice allows).
IN_PROC_BROWSER_TEST_F(RoamuxProfileCreationFlagOffBrowserTest,
                       UpstreamTypeChoicePreserved) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  const size_t initial_profiles = profile_manager->GetNumberOfProfiles();

  OpenPickerMainView();

  // Pass-through: the route resolves exactly as upstream's Dice value
  // dictates (the boolean reaches the WebUI unmodified when the flag is off).
  const bool dice_allowed =
      AccountConsistencyModeManager::IsDiceSignInAllowed();

  auto [step, type_choice_active] = ClickAddProfileAndReadState();
  if (dice_allowed) {
    EXPECT_EQ(step, "profileTypeChoice");
  } else {
    EXPECT_EQ(step, "localProfileCustomization");
  }

  // No profile is created behind the user's back on the type-choice path.
  if (dice_allowed) {
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ(profile_manager->GetNumberOfProfiles(), initial_profiles);
  }
}

}  // namespace
}  // namespace roamux
