// SPDX-License-Identifier: Apache-2.0
// roam-275: Roamux ships upstream kNewTabAddsToActiveGroup default-ON (patch
// 0067) — Cmd+T opens the new tab inside the active tab's group, at the
// group's end. Coverage contract (pinning boundary per the roam-226-era
// convention and roamux_features_unittest.cc precedent): exactly ONE test runs
// unpinned — the compiled-default sentinel, whose subject IS the default
// (RoamuxBrowserTest passes --disable-field-trial-config, roam-240, so the
// sentinel sees the real compiled default, not the field-trial testing
// config). All behavior-as-such tests pin the feature explicitly: an
// enabled fixture proving group-end insertion (positionally distinguishable —
// the topology keeps a trailing ungrouped tab so the group clamp inserts
// BEFORE it, upstream browser_tab_strip_controller_browsertest.cc precedent)
// and a disabled fixture proving the opt-out restores end-of-strip appends.
// The ungrouped-active-tab case guards the issue's explicit non-goal: no
// adjacent-to-active placement for Cmd+T.

#include <optional>

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "components/tab_groups/tab_group_id.h"
#include "content/public/test/browser_test.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// Tab topology shared by every behavioral test: [0] ungrouped, [1][2] in the
// returned group, [3] ungrouped trailing. The trailing tab is what makes the
// group-range clamp observable: an in-group insertion lands at index 3
// (before it), a plain append at index 4 (after it).
tab_groups::TabGroupId SetUpFourTabTopology(Browser* browser) {
  for (int i = 0; i < 3; ++i) {
    chrome::AddTabAt(browser, GURL("about:blank"), /*idx=*/-1,
                     /*foreground=*/true);
  }
  TabStripModel* const model = browser->tab_strip_model();
  EXPECT_EQ(4, model->count());
  return model->AddToNewGroup({1, 2});
}

// Unpinned ON PURPOSE: this fixture asserts the compiled default itself.
using RoamuxNewTabActiveGroupDefaultBrowserTest = test::RoamuxBrowserTest;

// The RED→GREEN sentinel for patch 0067: fails while the upstream default is
// DISABLED, passes once the patch flips it.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabActiveGroupDefaultBrowserTest,
                       FeatureIsEnabledByDefault) {
  EXPECT_TRUE(base::FeatureList::IsEnabled(features::kNewTabAddsToActiveGroup));
}

class RoamuxNewTabActiveGroupEnabledBrowserTest
    : public test::RoamuxBrowserTest {
 public:
  RoamuxNewTabActiveGroupEnabledBrowserTest() {
    feature_list_.InitAndEnableFeature(features::kNewTabAddsToActiveGroup);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

IN_PROC_BROWSER_TEST_F(RoamuxNewTabActiveGroupEnabledBrowserTest,
                       NewTabJoinsActiveTabsGroup) {
  TabStripModel* const model = browser()->tab_strip_model();
  const tab_groups::TabGroupId group = SetUpFourTabTopology(browser());

  model->ActivateTabAt(1);
  chrome::ExecuteCommand(browser(), IDC_NEW_TAB);

  ASSERT_EQ(5, model->count());
  // Inserted inside the group, at its end — index 3, before the trailing
  // ungrouped tab (which shifted to 4). A plain append would be index 4.
  EXPECT_EQ(3, model->active_index());
  EXPECT_EQ(group, model->GetTabGroupForTab(3));
  EXPECT_EQ(std::nullopt, model->GetTabGroupForTab(4));
  EXPECT_EQ(std::nullopt, model->GetTabGroupForTab(0));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabActiveGroupEnabledBrowserTest,
                       UngroupedActiveTabAppendsAtStripEnd) {
  TabStripModel* const model = browser()->tab_strip_model();
  SetUpFourTabTopology(browser());

  model->ActivateTabAt(0);
  chrome::ExecuteCommand(browser(), IDC_NEW_TAB);

  ASSERT_EQ(5, model->count());
  // The issue's non-goal guard: an ungrouped active tab gets no
  // adjacent-to-active placement and no group — plain end-of-strip append.
  EXPECT_EQ(4, model->active_index());
  EXPECT_EQ(std::nullopt, model->GetTabGroupForTab(4));
}

class RoamuxNewTabActiveGroupDisabledBrowserTest
    : public test::RoamuxBrowserTest {
 public:
  RoamuxNewTabActiveGroupDisabledBrowserTest() {
    feature_list_.InitAndDisableFeature(features::kNewTabAddsToActiveGroup);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

// The opt-out contract: disabling the feature (chrome://flags /
// --disable-features) restores the stock end-of-strip append even when the
// active tab is grouped.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabActiveGroupDisabledBrowserTest,
                       DisabledRestoresEndOfStripBehavior) {
  TabStripModel* const model = browser()->tab_strip_model();
  SetUpFourTabTopology(browser());

  model->ActivateTabAt(1);
  chrome::ExecuteCommand(browser(), IDC_NEW_TAB);

  ASSERT_EQ(5, model->count());
  EXPECT_EQ(4, model->active_index());
  EXPECT_EQ(std::nullopt, model->GetTabGroupForTab(4));
}

}  // namespace
}  // namespace roamux
