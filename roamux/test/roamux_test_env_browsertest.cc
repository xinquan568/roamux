// SPDX-License-Identifier: Apache-2.0
// roam-99 env-guard (TDD: born RED pre-hardening — see the RED commit):
// overlay browsertests must never run with the upstream WebUI-toolbar
// experiment enabled (browser_tests' field-trial testing config enables a
// sub-feature, arming an initial-paint wait on a surface the overlay does
// not use). GREEN via RoamuxBrowserTest's constructor-time ScopedFeatureList.
// (The RED commit asserted via --disable-features on the command line;
// BrowserTestBase DCHECKs that switch away — overrides must ride
// ScopedFeatureList — so the guard asserts feature state directly.)
// Also pins drift at uprevs: if the wait gate becomes reachable again, this
// fails loudly — re-derive the disable list from the pin's
// IsWebUIToolbarEnabled().

#include "base/feature_list.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/chrome_features.h"
#include "content/public/test/browser_test.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::test {
namespace {

using RoamuxTestEnvBrowserTest = RoamuxBrowserTest;

IN_PROC_BROWSER_TEST_F(RoamuxTestEnvBrowserTest,
                       WebUIToolbarExperimentIsOffInOverlayTests) {
  // The wait gate itself (the nine-feature OR) must be unreachable.
  EXPECT_FALSE(features::IsWebUIToolbarEnabled());
  // The member the field-trial testing config actually enables at this pin
  // (WebUIReloadButtonStudy) — the live threat, by exported constant.
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kWebUIReloadButton));
  // The independent wait trigger honored by webui_test_utils.cc.
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      features::kWebUIToolbarProcessOverheadExperiment));
}

}  // namespace
}  // namespace roamux::test
