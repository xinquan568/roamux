// SPDX-License-Identifier: Apache-2.0
// roam-31 (I-5.3): the opt-in entry point + three-build-state honesty.
// Flag-on + --roamex-signin-opt-in re-exposes on startup (the flags mirror);
// the settings toggle row is visible while suppressed and writes the pref;
// on this keyless reference build every sign-in initiation path is
// intercepted with the explanation (never a Gaia tab); flag-off untouched.
// TDD/P6: authored and run RED against the unpatched tree before patch 0024.

#include <string>

#include "base/strings/stringprintf.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/signin_ui_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/base/signin_metrics.h"
#include "components/signin/public/base/signin_pref_names.h"
#include "components/signin/public/identity_manager/account_info.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamex/browser/signin/signin_inert.h"
#include "roamex/common/roamex_features.h"
#include "roamex/common/roamex_prefs.h"
#include "roamex/common/roamex_switches.h"
#include "url/gurl.h"

namespace roamex {
namespace {

// Deep-queries open shadow roots for an element id, polling; optionally
// clicks it. Returns whether it was found.
constexpr char kDeepQueryScript[] = R"(
    (async () => {
      const deepQuery = (root, id) => {
        const direct = root.querySelector('#' + id);
        if (direct) return direct;
        for (const el of root.querySelectorAll('*')) {
          if (el.shadowRoot) {
            const hit = deepQuery(el.shadowRoot, id);
            if (hit) return hit;
          }
        }
        return null;
      };
      for (let i = 0; i < %d; i++) {
        const el = deepQuery(document, '%s');
        if (el) { if (%s) el.click(); return true; }
        await new Promise(r => setTimeout(r, 100));
      }
      return false;
    })();
)";

std::string DeepQueryScript(const std::string& id, int attempts, bool click) {
  return base::StringPrintf(kDeepQueryScript, attempts, id.c_str(),
                            click ? "true" : "false");
}

class RoamexSigninOptInTest : public InProcessBrowserTest {
 public:
  RoamexSigninOptInTest() {
    features_.InitAndEnableFeature(roamex::features::kBraveStyleProfiles);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(roamex::switches::kSigninOptIn);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    roamex::signin::SuppressInertExplanationDialogForTesting();
    roamex::signin::ResetInertExplanationShownForTesting();
  }

 private:
  base::test::ScopedFeatureList features_;
};

// Flag on, NO opt-in switch — the toggle row must be reachable while
// suppression is active.
class RoamexSigninOptInSuppressedTest : public InProcessBrowserTest {
 public:
  RoamexSigninOptInSuppressedTest() {
    features_.InitAndEnableFeature(roamex::features::kBraveStyleProfiles);
  }

 private:
  base::test::ScopedFeatureList features_;
};

class RoamexSigninOptInFlagOffTest : public InProcessBrowserTest {
 public:
  RoamexSigninOptInFlagOffTest() {
    features_.InitAndDisableFeature(roamex::features::kBraveStyleProfiles);
  }

 private:
  base::test::ScopedFeatureList features_;
};

// The flags mirror re-exposes at startup: the switch acts as the opt-in.
IN_PROC_BROWSER_TEST_F(RoamexSigninOptInTest, SwitchReexposesOnStartup) {
  EXPECT_TRUE(
      browser()->profile()->GetPrefs()->GetBoolean(::prefs::kSigninAllowed));
}

IN_PROC_BROWSER_TEST_F(RoamexSigninOptInSuppressedTest,
                       NoOptInStaysSuppressed) {
  EXPECT_FALSE(
      browser()->profile()->GetPrefs()->GetBoolean(::prefs::kSigninAllowed));
}

// The settings toggle row is present while suppressed, and clicking it writes
// roamex.signin.optional_entry_point_enabled.
IN_PROC_BROWSER_TEST_F(RoamexSigninOptInSuppressedTest,
                       SettingsToggleWritesOptInPref) {
  PrefService* prefs = browser()->profile()->GetPrefs();
  ASSERT_FALSE(prefs->GetBoolean(roamex::prefs::kSigninOptionalEntryPoint));

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://settings/appearance")));
  content::WebContents* settings =
      browser()->tab_strip_model()->GetActiveWebContents();

  ASSERT_EQ(true, content::EvalJs(settings, DeepQueryScript("roamexSigninOptIn",
                                                            /*attempts=*/50,
                                                            /*click=*/true)));
  // The settings-prefs binding commits asynchronously; poll the pref.
  while (!prefs->GetBoolean(roamex::prefs::kSigninOptionalEntryPoint)) {
    base::RunLoop().RunUntilIdle();
  }
  SUCCEED();
}

// Real Settings People path: the page's start-sign-in message (the exact
// handler the sign-in button posts) must be intercepted on this keyless
// build — no Gaia tab, explanation recorded.
IN_PROC_BROWSER_TEST_F(RoamexSigninOptInTest,
                       SettingsPeopleStartSigninIsIntercepted) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                           GURL("chrome://settings/people")));
  content::WebContents* settings =
      browser()->tab_strip_model()->GetActiveWebContents();
  const int tabs_before = browser()->tab_strip_model()->count();

  // The handler takes one arg: the page's ChromeSigninAccessPoint (0 =
  // settings), exactly what the sign-in button posts.
  ASSERT_TRUE(
      content::ExecJs(settings, "chrome.send('SyncSetupStartSignIn', [0]);"));
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(browser()->tab_strip_model()->count(), tabs_before);
  EXPECT_TRUE(roamex::signin::WasInertExplanationShownForTesting());
}

// The profile-menu invocation (its exact call + access point) and the direct
// multi-account entry are intercepted too.
IN_PROC_BROWSER_TEST_F(RoamexSigninOptInTest, FunnelEntriesAreIntercepted) {
  Profile* profile = browser()->profile();
  const int tabs_before = browser()->tab_strip_model()->count();

  roamex::signin::ResetInertExplanationShownForTesting();
  signin_ui_util::EnableSyncFromSingleAccountPromo(
      profile, CoreAccountInfo(),
      signin_metrics::AccessPoint::kAvatarBubbleSignIn);
  EXPECT_TRUE(roamex::signin::WasInertExplanationShownForTesting());

  roamex::signin::ResetInertExplanationShownForTesting();
  signin_ui_util::EnableSyncFromMultiAccountPromo(
      profile, CoreAccountInfo(), signin_metrics::AccessPoint::kSettings,
      /*is_default_promo_account=*/false);
  EXPECT_TRUE(roamex::signin::WasInertExplanationShownForTesting());

  roamex::signin::ResetInertExplanationShownForTesting();
  signin_ui_util::SignInFromSingleAccountPromo(
      profile, CoreAccountInfo(), signin_metrics::AccessPoint::kBookmarkBubble);
  EXPECT_TRUE(roamex::signin::WasInertExplanationShownForTesting());

  EXPECT_EQ(browser()->tab_strip_model()->count(), tabs_before);
}

// Flag off: no interception, no settings row.
IN_PROC_BROWSER_TEST_F(RoamexSigninOptInFlagOffTest, NoRowNoInterception) {
  EXPECT_FALSE(roamex::signin::ShouldInterceptInertSignin());

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://settings/appearance")));
  content::WebContents* settings =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_EQ(false,
            content::EvalJs(settings, DeepQueryScript("roamexSigninOptIn",
                                                      /*attempts=*/15,
                                                      /*click=*/false)));
}

}  // namespace
}  // namespace roamex
