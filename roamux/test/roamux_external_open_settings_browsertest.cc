// SPDX-License-Identifier: Apache-2.0
// roam-213: the manageProfile "Use this profile for external links and files"
// toggle (patch 0052) — flag-gated stamping, live pref binding, single-owner
// semantics. Shape: roamux_tab_strip_position_settings_browsertest.cc.

#include <string>

#include "base/strings/stringprintf.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// Deep-queries open shadow roots for an element id, polling until it appears
// (Polymer stamps dom-if templates asynchronously). Resolves true iff found
// within the deadline.
constexpr char kWaitForIdScript[] = R"(
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
        if (deepQuery(document, '%s')) return true;
        await new Promise(r => setTimeout(r, 100));
      }
      return false;
    })();
)";

std::string WaitForIdScript(const std::string& id, int attempts) {
  return base::StringPrintf(kWaitForIdScript, attempts, id.c_str());
}

// Runs `body` with `el` bound to the deep-queried #externalOpenToggle
// (assumes it exists).
std::string WithToggleScript(const std::string& body) {
  return base::StringPrintf(R"(
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
        const el = deepQuery(document, 'externalOpenToggle');
        if (!el) return 'missing';
        %s
      })();
  )",
                            body.c_str());
}

class RoamuxExternalOpenSettingsTest : public test::RoamuxBrowserTest {
 public:
  RoamuxExternalOpenSettingsTest() {
    features_.InitAndEnableFeature(features::kRoamuxExternalOpenProfile);
  }

 protected:
  content::WebContents* NavigateToManageProfile() {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(
        browser(), GURL("chrome://settings/manageProfile")));
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  PrefService* local_state() { return g_browser_process->local_state(); }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenSettingsTest,
                       ToggleWritesBothPrefsAndUnchecksToModeZero) {
  content::WebContents* web_contents = NavigateToManageProfile();
  ASSERT_EQ(
      content::EvalJs(web_contents, WaitForIdScript("externalOpenToggle", 50)),
      true);

  // Check: designates THIS profile (the default profile's dir base name).
  ASSERT_EQ(content::EvalJs(web_contents,
                            WithToggleScript("el.click(); return true;")),
            true);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return local_state()->GetInteger(prefs::kExternalOpenMode) == 1;
  }));
  EXPECT_EQ(local_state()->GetString(prefs::kExternalOpenProfile),
            chrome::kInitialProfile);

  // Uncheck: clears the mode; the stored name is inert at mode 0.
  ASSERT_EQ(content::EvalJs(web_contents,
                            WithToggleScript("el.click(); return true;")),
            true);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return local_state()->GetInteger(prefs::kExternalOpenMode) == 0;
  }));
  EXPECT_EQ(local_state()->GetString(prefs::kExternalOpenProfile),
            chrome::kInitialProfile);
}

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenSettingsTest,
                       DesignatingAnotherProfileUnchecksThisOne) {
  // Designate THIS profile first, then hand ownership to another base name —
  // the toggle must compute unchecked (single-owner semantics come from the
  // single Local State pref pair).
  local_state()->SetInteger(prefs::kExternalOpenMode, 1);
  local_state()->SetString(prefs::kExternalOpenProfile,
                           chrome::kInitialProfile);
  content::WebContents* web_contents = NavigateToManageProfile();
  ASSERT_EQ(
      content::EvalJs(web_contents, WaitForIdScript("externalOpenToggle", 50)),
      true);
  ASSERT_EQ(
      content::EvalJs(web_contents,
                      WithToggleScript("return el.hasAttribute('checked');")),
      true);

  local_state()->SetString(prefs::kExternalOpenProfile, "Profile Roam213 X");
  ASSERT_EQ(content::EvalJs(web_contents, WithToggleScript(base::StringPrintf(
                                              R"(for (let i = 0; i < %d; i++) {
                                     if (!el.hasAttribute('checked')) {
                                       return true;
                                     }
                                     await new Promise(r => setTimeout(r, 100));
                                   }
                                   return false;)",
                                              50))),
            true);
}

class RoamuxExternalOpenSettingsFlagOffTest : public test::RoamuxBrowserTest {
 public:
  RoamuxExternalOpenSettingsFlagOffTest() {
    features_.InitAndDisableFeature(features::kRoamuxExternalOpenProfile);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenSettingsFlagOffTest,
                       ToggleAbsentWhenFlagOff) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://settings/manageProfile")));
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  // The dom-if must not stamp: bounded poll returns false (element absent).
  ASSERT_EQ(
      content::EvalJs(web_contents, WaitForIdScript("externalOpenToggle", 20)),
      false);
}

}  // namespace
}  // namespace roamux
