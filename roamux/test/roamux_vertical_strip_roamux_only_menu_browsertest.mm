// SPDX-License-Identifier: Apache-2.0
// roam-239: read-side consumer coverage for the widened
// tabs::IsVerticalTabsFeatureEnabled() predicate. The View menu's
// vertical-tabs toggle is built with remove_if(!predicate)
// (main_menu_builder.mm), so under the product-intended Roamux-only
// configuration (kTabStripPosition on, upstream vertical-tabs features off)
// the item exists only if the predicate includes the Roamux capability.
// TDD: written RED before the predicate patch — the item was absent. The
// explicit disabled set is load-bearing: it overrides the compiled-in
// field-trial testing config (see the roam-234 startup fixture). The
// assertion is command-tag-only — AppController retitles the item on
// vertical-mode changes, so the title is deliberately never asserted.

#import <Cocoa/Cocoa.h>

#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/features.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"

namespace roamux {
namespace {

// kTabStripPosition ON, both upstream vertical-tabs features OFF — the
// configuration the product ships (mirrors
// RoamuxVerticalStripRoamuxOnlyStartupTest).
class RoamuxVerticalStripRoamuxOnlyMenuTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripRoamuxOnlyMenuTest() {
    features_.InitWithFeatures(
        /*enabled_features=*/{features::kTabStripPosition},
        /*disabled_features=*/{::tabs::kVerticalTabs,
                               ::tabs::kVerticalTabsLaunch});
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripRoamuxOnlyMenuTest,
                       ViewMenuCarriesVerticalTabsToggle) {
  ASSERT_TRUE(browser());
  ASSERT_TRUE(browser()->window());
  NSMenu* view_submenu = [[NSApp.mainMenu itemWithTag:IDC_VIEW_MENU] submenu];
  ASSERT_TRUE(view_submenu != nil);
  // Built by main_menu_builder as
  // Item(IDS_SWITCH_TO_VERTICAL_TAB).command_id(IDC_TOGGLE_VERTICAL_TABS)
  //     .remove_if(!tabs::IsVerticalTabsFeatureEnabled());
  // command_id() sets the item tag, so the lookup is title-independent.
  EXPECT_TRUE([view_submenu itemWithTag:IDC_TOGGLE_VERTICAL_TABS] != nil);
}

}  // namespace
}  // namespace roamux
