// SPDX-License-Identifier: Apache-2.0
// roam-140/roam-160: runs the chrome://settings/help Roamux About WebUI mocha
// tests — the branded about_page integration suite (roamux_settings_about_test
// .ts) and the left-nav chip suite (roam-157). The update card and its suite
// were retired by roam-160 (updates render in the NATIVE row; per-state
// coverage lives in roamux/test/roamux_update_row_browsertest.cc, tier-2).

#include "base/strings/stringprintf.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/test/base/web_ui_mocha_browser_test.h"
#include "content/public/test/browser_test.h"
#include "roamux/test/support/roamux_browser_test.h"

class RoamuxSettingsAboutBrowserTest : public WebUIMochaBrowserTest {
 public:
  RoamuxSettingsAboutBrowserTest() {
    set_test_loader_host(chrome::kChromeUISettingsHost);
    // roam-99: foreign hierarchy (WebUIMochaBrowserTest, hosted in upstream
    // browser_tests) — fixture-owned ScopedFeatureList via the header-only
    // helper instead of re-basing.
    roamux::test::DisableWebUIToolbarFeatures(webui_toolbar_disables_);
  }

 protected:
  void RunAboutTestCase(const std::string& test_case) {
    RunTest("roamux_settings_about/roamux_settings_about_test.js",
            base::StringPrintf("runMochaTest('RoamuxSettingsAbout', '%s');",
                               test_case.c_str()));
  }

  // roam-157: the settings left-nav About chip (settings-menu element).
  void RunMenuTestCase(const std::string& test_case) {
    RunTest("roamux_settings_about/roamux_settings_menu_test.js",
            base::StringPrintf("runMochaTest('RoamuxSettingsMenu', '%s');",
                               test_case.c_str()));
  }

 private:
  base::test::ScopedFeatureList webui_toolbar_disables_;
};

// --- branded chrome://settings/help integration suite ---

IN_PROC_BROWSER_TEST_F(RoamuxSettingsAboutBrowserTest, BrandedLogoAndTitle) {
  RunAboutTestCase("branded logo and title render");
}

// roam-156 left this mocha case without a C++ runner, so it never executed. roam-157
// touches the same About surface (hero logo -> .svg); wire it in while here.
IN_PROC_BROWSER_TEST_F(RoamuxSettingsAboutBrowserTest, AboutVersionLines) {
  RunAboutTestCase("version lines name Roamux and Chromium separately");
}


// --- settings nav About chip suite (roam-157) ---

IN_PROC_BROWSER_TEST_F(RoamuxSettingsAboutBrowserTest, MenuChipGlyph) {
  RunMenuTestCase("about chip shows the roamux glyph");
}

IN_PROC_BROWSER_TEST_F(RoamuxSettingsAboutBrowserTest, MenuChipTwoLines) {
  RunMenuTestCase("about chip renders two lines: title over v-version");
}

IN_PROC_BROWSER_TEST_F(RoamuxSettingsAboutBrowserTest, MenuChipSiblingsSingleLine) {
  RunMenuTestCase("other nav items keep a single line");
}
