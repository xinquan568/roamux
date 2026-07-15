// SPDX-License-Identifier: Apache-2.0
// roam-99: overlay browsertests must not depend on the upstream WebUI-toolbar
// experiment. browser_tests' default field-trial testing config enables one of
// its sub-features (WebUIReloadButtonStudy at pin M149), which arms
// WaitUntilInitialWebUIPaintAndFlushMetricsForTesting at every browser start —
// a wait on a surface the overlay never uses, and a hang whenever that
// surface's resources break (as roam-94's rename rebuild showed). Overlay test
// setup therefore disables the whole IsWebUIToolbarEnabled() OR-set plus the
// independent process-overhead wait trigger.
//
// Mechanism note: BrowserTestBase DCHECKs against a bare --disable-features
// switch (browser_test_base.cc) — feature overrides in browser tests must ride
// base::test::ScopedFeatureList, initialized BEFORE SetUp() (i.e. in the
// fixture constructor). ScopedFeatureList overrides win over the field-trial
// testing config.
//
// HEADER-ONLY helper on purpose: the overlay's WebUI mocha fixtures are
// compiled into upstream browser_tests, where adding a roamux link dependency
// is not an option. Fixtures on foreign hierarchies (ProfilePickerTestBase,
// WebUIMochaBrowserTest, ...) hold their own ScopedFeatureList member and call
// DisableWebUIToolbarFeatures on it from their constructor;
// InProcessBrowserTest fixtures simply derive RoamuxBrowserTest.

#ifndef ROAMUX_TEST_SUPPORT_ROAMUX_BROWSER_TEST_H_
#define ROAMUX_TEST_SUPPORT_ROAMUX_BROWSER_TEST_H_

#include "base/test/scoped_feature_list.h"
#include "chrome/test/base/in_process_browser_test.h"

namespace roamux::test {

// The pin's chrome-side WebUI-toolbar sub-feature set — the
// IsWebUIToolbarEnabled() OR (chrome/browser/ui/ui_features.cc) — plus
// kWebUIToolbarProcessOverheadExperiment, which webui_test_utils.cc honors as
// an independent wait trigger. PIN-RELATIVE: re-derive from the pin's
// IsWebUIToolbarEnabled() definition at every uprev (the roam-99 env-guard
// browsertest fails loudly if the gate is reachable again).
inline constexpr char kRoamuxDisabledWebUIToolbarFeatures[] =
    "WebUIReloadButton,WebUISplitTabsButton,WebUIHomeButton,WebUILocationBar,"
    "WebUIBackForwardButton,WebUIPinnedToolbarActions,"
    "WebUIExtensionsContainer,WebUIAvatarButton,WebUIAppMenuButton,"
    "WebUIToolbarProcessOverheadExperiment";

// Call from a fixture CONSTRUCTOR on a fixture-owned ScopedFeatureList.
inline void DisableWebUIToolbarFeatures(
    base::test::ScopedFeatureList& feature_list) {
  feature_list.InitFromCommandLine(
      /*enable_features=*/"",
      /*disable_features=*/kRoamuxDisabledWebUIToolbarFeatures);
}

// The overlay browsertest base: InProcessBrowserTest with the WebUI-toolbar
// experiment pinned off (constructor-time ScopedFeatureList; body in the .cc —
// chromium-style forbids inline complex constructors).
class RoamuxBrowserTest : public InProcessBrowserTest {
 public:
  RoamuxBrowserTest();
  ~RoamuxBrowserTest() override;

 private:
  base::test::ScopedFeatureList webui_toolbar_disables_;
};

}  // namespace roamux::test

#endif  // ROAMUX_TEST_SUPPORT_ROAMUX_BROWSER_TEST_H_
