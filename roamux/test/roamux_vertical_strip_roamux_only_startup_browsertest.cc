// SPDX-License-Identifier: Apache-2.0
// roam-234: the product-intended Roamux-only configuration (kTabStripPosition
// on, upstream vertical-tabs features off) must start the browser. Before
// patch 0059 the collapse-action producer (browser_actions.cc) still gated on
// the upstream feature alone while patch 0008 had widened the strip-creation
// gate, so VerticalTabStripTopContainer::AddChildButtonFor CHECK-crashed
// during BrowserView construction — before any test body runs. (TDD: written
// RED before patch 0059.) The explicit disabled set is load-bearing: it
// overrides the compiled-in field-trial testing config, whose VerticalTabs /
// VerticalTabsLaunch studies enable the upstream features on mac and mask the
// defect in every default build and every other E1 fixture.

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/tabs/features.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/actions/actions.h"

namespace roamux {
namespace {

// roam-206 accessor shape: the collapse action item under this browser's
// root action item, or null if the producer never registered it.
actions::ActionItem* CollapseActionItem(Browser* browser) {
  return actions::ActionManager::Get().FindAction(
      kActionToggleCollapseVertical,
      browser->browser_actions()->root_action_item());
}

// kTabStripPosition ON, both upstream vertical-tabs features OFF — the
// configuration the product ships. Browser bring-up itself is the regression
// surface.
class RoamuxVerticalStripRoamuxOnlyStartupTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripRoamuxOnlyStartupTest() {
    features_.InitWithFeatures(
        /*enabled_features=*/{features::kTabStripPosition},
        /*disabled_features=*/{::tabs::kVerticalTabs,
                               ::tabs::kVerticalTabsLaunch});
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripRoamuxOnlyStartupTest,
                       StartsWithRoamuxOnlyConfiguration) {
  ASSERT_TRUE(browser());
  ASSERT_TRUE(browser()->window());
  // The producer must have run: the strip's top container binds its collapse
  // button to this action during BrowserView construction.
  EXPECT_NE(CollapseActionItem(browser()), nullptr);
}

// roam-239: the predicate itself is the read-side contract — production call
// sites from session/tab restore through the mac menus, theme pack, metrics,
// settings strings, and the state controller key off
// IsVerticalTabsFeatureEnabled() alone. Under the Roamux-only configuration it
// returned false while the patch-0008 creation gate built the strip anyway;
// the widened predicate must report the capability the strip actually has.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripRoamuxOnlyStartupTest,
                       PredicateIncludesRoamuxCapability) {
  EXPECT_TRUE(::tabs::IsVerticalTabsFeatureEnabled());
}

// All three features OFF: the patched producer condition must degrade to the
// upstream expression exactly — no action registered, clean startup.
class RoamuxVerticalStripAllOffStartupTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripAllOffStartupTest() {
    features_.InitWithFeatures(
        /*enabled_features=*/{},
        /*disabled_features=*/{features::kTabStripPosition,
                               ::tabs::kVerticalTabs,
                               ::tabs::kVerticalTabsLaunch});
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripAllOffStartupTest,
                       UpstreamParityWhenRoamuxFlagOff) {
  ASSERT_TRUE(browser());
  ASSERT_TRUE(browser()->window());
  EXPECT_EQ(CollapseActionItem(browser()), nullptr);
  // roam-239 parity guard: with every feature off the widened predicate must
  // degrade to the upstream expression exactly.
  EXPECT_FALSE(::tabs::IsVerticalTabsFeatureEnabled());
}

}  // namespace
}  // namespace roamux
