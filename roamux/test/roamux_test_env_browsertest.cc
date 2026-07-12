// SPDX-License-Identifier: Apache-2.0
// roam-99 env-guard (TDD: written RED before the hardening): overlay
// browsertests must never run with the upstream WebUI-toolbar experiment
// enabled. RED on an unhardened build because browser_tests' default
// field-trial testing config (WebUIReloadButtonStudy at pin M149) enables
// WebUIReloadButton, arming an initial-paint wait on a surface the overlay
// does not use.

#include <algorithm>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/strings/string_split.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::test {
namespace {

// The pin's IsWebUIToolbarEnabled() OR-set plus the independent
// process-overhead wait trigger (webui_test_utils.cc). Re-derive at uprev.
constexpr char kExpectedDisabledFeatures[] =
    "WebUIReloadButton,WebUISplitTabsButton,WebUIHomeButton,WebUILocationBar,"
    "WebUIBackForwardButton,WebUIPinnedToolbarActions,"
    "WebUIExtensionsContainer,WebUIAvatarButton,WebUIAppMenuButton,"
    "WebUIToolbarProcessOverheadExperiment";

using RoamuxTestEnvBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(RoamuxTestEnvBrowserTest,
                       WebUIToolbarExperimentIsOffInOverlayTests) {
  // The wait gate itself (the nine-feature OR) must be unreachable.
  EXPECT_FALSE(features::IsWebUIToolbarEnabled());

  // Item-by-item: every listed feature rides --disable-features in the test
  // process (command-line disables override the field-trial testing config).
  const std::string disabled =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "disable-features");
  const std::vector<std::string> disabled_items = base::SplitString(
      disabled, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (const std::string& feature :
       base::SplitString(kExpectedDisabledFeatures, ",", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    EXPECT_TRUE(std::ranges::find(disabled_items, feature) !=
                disabled_items.end())
        << feature << " is not disabled in overlay browsertests";
  }
}

}  // namespace
}  // namespace roamux::test
