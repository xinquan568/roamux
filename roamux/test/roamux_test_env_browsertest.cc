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
#include "chrome/browser/feedback/feedback_uploader_factory_chrome.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/features.h"
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

// roam-223 env-guard (TDD: born RED — see the RED commit): the overlay suite
// must never instantiate FeedbackUploaderChrome. Its constructor posts
// FeedbackReport::LoadReportsAndQueue on a
// {MayBlock, BEST_EFFORT, BLOCK_SHUTDOWN} runner
// (feedback_uploader_chrome.cc:69-75, feedback_uploader.cc:76-80); a
// BEST_EFFORT task may never get a background worker during a short test, and
// at shutdown it can sit queued-but-not-running while CompleteShutdown waits —
// the attributed cause of the roam-201/roam-223 teardown stall
// ("running tasks: none" + a BEST_EFFORT registration from
// feedback_uploader_chrome.cc:70).
//
// Upstream intends this service to be absent in tests
// (ServiceIsNULLWhileTesting() == true) but that only applies to the
// CreateBrowserContextServicesForTest() path used by TestingProfile; browser
// tests build a real ProfileImpl through the production path, so the service
// is created eagerly for every browser-test profile
// (ServiceIsCreatedWithBrowserContext() == true). GREEN via
// RoamuxBrowserTest::SetUpBrowserContextKeyedServices installing an empty
// testing factory.
//
// Also pins drift at uprevs: if the suppression hook moves or the factory is
// reshaped, this fails loudly instead of silently no-op'ing.
IN_PROC_BROWSER_TEST_F(RoamuxTestEnvBrowserTest,
                       FeedbackUploaderIsNotInstantiatedInOverlayTests) {
  EXPECT_EQ(nullptr,
            feedback::FeedbackUploaderFactoryChrome::GetForBrowserContext(
                browser()->profile()));
}

// roam-240 env-guard (TDD: born RED pre-switch): overlay browsertests must run
// real flag defaults, not the compiled-in field-trial testing config (1,291
// studies at this pin). The config's VerticalTabs/VerticalTabsLaunch studies
// masked the whole E1 gate-mismatch family (roam-234's startup crash,
// roam-239's ten read-side sites) until fixtures with explicit disables
// existed. GREEN via RoamuxBrowserTest::SetUpCommandLine appending
// --disable-field-trial-config, after which a default-off upstream feature
// reads disabled from a plain derived fixture. A test that wants an upstream
// feature opts in via ScopedFeatureList — explicit and self-documenting.
// Also pins drift: if the switch is lost in a refactor or the mechanism
// changes at an uprev, this fails loudly.
IN_PROC_BROWSER_TEST_F(RoamuxTestEnvBrowserTest,
                       FieldTrialTestingConfigIsOffInOverlayTests) {
  // Default-off upstream feature that the testing config's VerticalTabs study
  // enables at this pin — the exact masking that hid roam-234/roam-239.
  EXPECT_FALSE(base::FeatureList::IsEnabled(::tabs::kVerticalTabs));
}

}  // namespace
}  // namespace roamux::test
