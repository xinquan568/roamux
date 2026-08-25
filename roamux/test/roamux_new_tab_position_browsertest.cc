// SPDX-License-Identifier: Apache-2.0
// roam-277: roamux.tabs.new_tab_position — where an EXPLICITLY created blank
// tab (Cmd+T, "+", File > New Tab; every chrome::NewTab() route except
// NewTabTypes::kNoUserAction) is inserted: end of strip / end of active group
// / after active tab. The seam is chrome::NewTab() (patch 0068), gated on
// roamux::features::kNewTabPosition; TabStripModel is untouched, so every
// pinned / split / group-range clamp asserted here is upstream's.
//
// Pinning contract (roam-226-era convention): exactly TWO tests run unpinned —
// the compiled-default sentinel for the feature and the registered-default
// sentinel for the pref. Every other fixture initialises ONE constructor-time
// ScopedFeatureList with the complete relevant state (kNewTabPosition AND the
// upstream kNewTabAddsToActiveGroup, whose default patch 0067 flipped) and
// every test sets the pref explicitly before its first action.
//
// Topologies (identity via WebContents* captured at creation; the trailing
// ungrouped tab keeps group-end and strip-end positionally distinct):
//   T4  = [0]U [1]G [2]G [3]U      TG = [0]U [1]G [2]G (group last in strip)
//   T2  = [A][B]                   U4 = four ungrouped   T1G = one grouped tab
//
// Written RED before patch 0068 (TDD/P6): the expected pre-patch failure set
// is enumerated in the frozen plan; every other test here passes on the stock
// path by construction and guards it.

#include <optional>
#include <vector>

#include "base/feature_list.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/split_tab_metrics.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_view.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_utils.h"
#include "roamux/common/new_tab_position.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/common/tab_strip_placement.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace roamux {
namespace {

using ::tab_groups::TabGroupId;

class NewTabPositionTestBase : public test::RoamuxBrowserTest {
 protected:
  NewTabPositionTestBase(bool roamux_feature_on, bool upstream_feature_on) {
    std::vector<base::test::FeatureRef> enabled;
    std::vector<base::test::FeatureRef> disabled;
    (roamux_feature_on ? enabled : disabled)
        .push_back(features::kNewTabPosition);
    (upstream_feature_on ? enabled : disabled)
        .push_back(::features::kNewTabAddsToActiveGroup);
    features_.InitWithFeatures(enabled, disabled);
  }

  TabStripModel* model() { return browser()->tab_strip_model(); }
  PrefService* prefs() { return browser()->profile()->GetPrefs(); }
  void SetMode(NewTabPosition mode) { SetNewTabPosition(prefs(), mode); }

  void AddTabs(int count) {
    for (int i = 0; i < count; ++i) {
      chrome::AddTabAt(browser(), GURL(url::kAboutBlankURL), /*idx=*/-1,
                       /*foreground=*/true);
    }
  }
  // T4: [0]U [1]G [2]G [3]U.
  TabGroupId SetUpT4() {
    AddTabs(3);
    EXPECT_EQ(4, model()->count());
    return model()->AddToNewGroup({1, 2});
  }
  // TG: [0]U [1]G [2]G — the group is last in the strip.
  TabGroupId SetUpTG() {
    AddTabs(2);
    EXPECT_EQ(3, model()->count());
    return model()->AddToNewGroup({1, 2});
  }
  void SetUpT2() { AddTabs(1); }
  void SetUpU4() { AddTabs(3); }
  // T1G: the harness's single tab, grouped.
  TabGroupId SetUpT1G() {
    EXPECT_EQ(1, model()->count());
    return model()->AddToNewGroup({0});
  }

  content::WebContents* Contents(int index) {
    return model()->GetWebContentsAt(index);
  }
  int IndexOf(content::WebContents* contents) {
    return model()->GetIndexOfWebContents(contents);
  }
  std::optional<TabGroupId> GroupOf(content::WebContents* contents) {
    return model()->GetTabGroupForTab(IndexOf(contents));
  }
  // The Cmd+T / File > New Tab / app-menu route (IDC_NEW_TAB ->
  // NewTabTypes::kNewTabCommand).
  content::WebContents* NewTabViaCommand() {
    chrome::ExecuteCommand(browser(), IDC_NEW_TAB);
    return model()->GetActiveWebContents();
  }
  content::WebContents* NewTabViaType(NewTabTypes type) {
    return &chrome::NewTab(browser(), type);
  }
  void CloseAndWait(content::WebContents* contents) {
    content::WebContentsDestroyedWatcher watcher(contents);
    model()->CloseWebContentsAt(IndexOf(contents),
                                TabCloseTypes::CLOSE_USER_GESTURE);
    watcher.Wait();
  }
  // Typed navigation in the active tab — the transition TabNavigating
  // forgives only for a literally-last chrome://newtab (tab_strip_model.cc).
  void TypedNavigateActiveTab() {
    ASSERT_TRUE(
        ui_test_utils::NavigateToURL(browser(), GURL(url::kAboutBlankURL)));
  }
  void ExpectPlacement(content::WebContents* contents,
                       int index,
                       std::optional<TabGroupId> group) {
    EXPECT_EQ(index, IndexOf(contents));
    EXPECT_EQ(contents, model()->GetActiveWebContents());
    EXPECT_EQ(group, model()->GetTabGroupForTab(index));
  }

 private:
  base::test::ScopedFeatureList features_;
};

class RoamuxNewTabPositionTest : public NewTabPositionTestBase {
 public:
  RoamuxNewTabPositionTest()
      : NewTabPositionTestBase(/*roamux_feature_on=*/true,
                               /*upstream_feature_on=*/true) {}
};

class RoamuxNewTabPositionFlagOffTest : public NewTabPositionTestBase {
 public:
  RoamuxNewTabPositionFlagOffTest()
      : NewTabPositionTestBase(/*roamux_feature_on=*/false,
                               /*upstream_feature_on=*/true) {}
};

class RoamuxNewTabPositionUpstreamOffTest : public NewTabPositionTestBase {
 public:
  RoamuxNewTabPositionUpstreamOffTest()
      : NewTabPositionTestBase(/*roamux_feature_on=*/true,
                               /*upstream_feature_on=*/false) {}
};

// ---- the two unpinned sentinels -------------------------------------------

using RoamuxNewTabPositionDefaultTest = test::RoamuxBrowserTest;

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionDefaultTest,
                       FeatureIsEnabledByDefault) {
  EXPECT_TRUE(base::FeatureList::IsEnabled(features::kNewTabPosition));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionDefaultTest,
                       PrefDefaultsToEndOfActiveGroup) {
  PrefService* prefs = browser()->profile()->GetPrefs();
  EXPECT_EQ(1, prefs->GetInteger(prefs::kNewTabPosition));
  EXPECT_EQ(NewTabPosition::kEndOfActiveGroup, GetNewTabPosition(prefs));
}

// ---- mode 0: end_of_strip ---------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfStripGroupedMidGroupAppendsUngrouped) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(3));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfStripGroupedLastInGroupAppendsUngrouped) {
  SetUpT4();
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest, EndOfStripUngroupedAppends) {
  SetUpT4();
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(0);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfStripGroupLastInStripAppendsUngrouped) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(4, model()->count());
  ExpectPlacement(n, 3, std::nullopt);
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest, EndOfStripRepeatedOrdering) {
  const TabGroupId group = SetUpT4();
  content::WebContents* trailing = Contents(3);
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(1);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(1);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(7, model()->count());
  EXPECT_EQ(3, IndexOf(trailing));
  EXPECT_EQ(4, IndexOf(n1));
  EXPECT_EQ(5, IndexOf(n2));
  EXPECT_EQ(6, IndexOf(n3));
  for (content::WebContents* n : {n1, n2, n3}) {
    EXPECT_EQ(std::nullopt, GroupOf(n));
  }
  EXPECT_EQ(group, model()->GetTabGroupForTab(1));
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfStripRepeatedOrderingGroupLastInStrip) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(2);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(2);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(6, model()->count());
  EXPECT_EQ(3, IndexOf(n1));
  EXPECT_EQ(4, IndexOf(n2));
  EXPECT_EQ(5, IndexOf(n3));
  for (content::WebContents* n : {n1, n2, n3}) {
    EXPECT_EQ(std::nullopt, GroupOf(n));
  }
  EXPECT_EQ(group, model()->GetTabGroupForTab(1));
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(3));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfStripRepeatedOrderingUngrouped) {
  SetUpT2();
  content::WebContents* a = Contents(0);
  content::WebContents* b = Contents(1);
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(0);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(0);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  EXPECT_EQ(0, IndexOf(a));
  EXPECT_EQ(1, IndexOf(b));
  EXPECT_EQ(2, IndexOf(n1));
  EXPECT_EQ(3, IndexOf(n2));
  EXPECT_EQ(4, IndexOf(n3));
}

// ---- mode 1: end_of_active_group (the roam-275 / patch-0067 default) --------

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupGroupedMidGroup) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 3, group);
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(4));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupGroupedLastInGroup) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 3, group);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupUngroupedAppends) {
  SetUpT4();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(0);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupGroupLastInStrip) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(4, model()->count());
  ExpectPlacement(n, 3, group);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupRepeatedOrdering) {
  const TabGroupId group = SetUpT4();
  content::WebContents* trailing = Contents(3);
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(1);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(1);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(7, model()->count());
  EXPECT_EQ(3, IndexOf(n1));
  EXPECT_EQ(4, IndexOf(n2));
  EXPECT_EQ(5, IndexOf(n3));
  EXPECT_EQ(6, IndexOf(trailing));
  for (int i = 1; i <= 5; ++i) {
    EXPECT_EQ(group, model()->GetTabGroupForTab(i)) << i;
  }
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(6));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupRepeatedOrderingGroupLastInStrip) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(2);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(2);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(6, model()->count());
  EXPECT_EQ(3, IndexOf(n1));
  EXPECT_EQ(4, IndexOf(n2));
  EXPECT_EQ(5, IndexOf(n3));
  for (int i = 1; i <= 5; ++i) {
    EXPECT_EQ(group, model()->GetTabGroupForTab(i)) << i;
  }
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       EndOfActiveGroupRepeatedOrderingUngrouped) {
  SetUpT2();
  content::WebContents* a = Contents(0);
  content::WebContents* b = Contents(1);
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(0);
  content::WebContents* n1 = NewTabViaCommand();
  content::WebContents* n2 = NewTabViaCommand();
  model()->ActivateTabAt(0);
  content::WebContents* n3 = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  EXPECT_EQ(0, IndexOf(a));
  EXPECT_EQ(1, IndexOf(b));
  EXPECT_EQ(2, IndexOf(n1));
  EXPECT_EQ(3, IndexOf(n2));
  EXPECT_EQ(4, IndexOf(n3));
}

// ---- mode 2: after_active_tab (the new placement) ---------------------------

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabGroupedMidGroup) {
  const TabGroupId group = SetUpT4();
  content::WebContents* second_in_group = Contents(2);
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 2, group);
  EXPECT_EQ(3, IndexOf(second_in_group));
  EXPECT_EQ(group, model()->GetTabGroupForTab(3));
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(4));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabGroupedLastInGroup) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 3, group);
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(4));
}

// The adjacency rule (tab_strip_model.cc AddTab): a new tab requested with
// NO group adopts one only when BOTH neighbours share it. The ungrouped
// active tab is the left neighbour, so the new tab stays ungrouped even
// though [1] starts the group.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabUngroupedStaysUngroupedBeforeGroup) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(0);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 1, std::nullopt);
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_EQ(group, model()->GetTabGroupForTab(3));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabTrailingUngroupedAppends) {
  SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(3);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabGroupLastInStrip) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(4, model()->count());
  ExpectPlacement(n, 3, group);
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabRepeatedOrderingUngrouped) {
  SetUpT2();
  content::WebContents* a = Contents(0);
  content::WebContents* b = Contents(1);
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(0);
  content::WebContents* n1 = NewTabViaCommand();  // [A,N1,B]
  content::WebContents* n2 = NewTabViaCommand();  // [A,N1,N2,B]
  model()->ActivateTabAt(0);
  content::WebContents* n3 = NewTabViaCommand();  // [A,N3,N1,N2,B]
  ASSERT_EQ(5, model()->count());
  EXPECT_EQ(0, IndexOf(a));
  EXPECT_EQ(1, IndexOf(n3));
  EXPECT_EQ(2, IndexOf(n1));
  EXPECT_EQ(3, IndexOf(n2));
  EXPECT_EQ(4, IndexOf(b));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabRepeatedOrderingGrouped) {
  const TabGroupId group = SetUpT4();
  content::WebContents* first_in_group = Contents(1);
  content::WebContents* second_in_group = Contents(2);
  content::WebContents* trailing = Contents(3);
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n1 = NewTabViaCommand();  // G=[1,N1,2]
  content::WebContents* n2 = NewTabViaCommand();  // G=[1,N1,N2,2]
  model()->ActivateTabAt(1);
  content::WebContents* n3 = NewTabViaCommand();  // G=[1,N3,N1,N2,2]
  ASSERT_EQ(7, model()->count());
  EXPECT_EQ(1, IndexOf(first_in_group));
  EXPECT_EQ(2, IndexOf(n3));
  EXPECT_EQ(3, IndexOf(n1));
  EXPECT_EQ(4, IndexOf(n2));
  EXPECT_EQ(5, IndexOf(second_in_group));
  EXPECT_EQ(6, IndexOf(trailing));
  for (int i = 1; i <= 5; ++i) {
    EXPECT_EQ(group, model()->GetTabGroupForTab(i)) << i;
  }
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(6));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabRepeatedOrderingGroupLastInStrip) {
  const TabGroupId group = SetUpTG();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(2);
  content::WebContents* n1 = NewTabViaCommand();  // G=[1,2,N1]
  content::WebContents* n2 = NewTabViaCommand();  // G=[1,2,N1,N2]
  model()->ActivateTabAt(2);
  content::WebContents* n3 = NewTabViaCommand();  // G=[1,2,N3,N1,N2]
  ASSERT_EQ(6, model()->count());
  EXPECT_EQ(3, IndexOf(n3));
  EXPECT_EQ(4, IndexOf(n1));
  EXPECT_EQ(5, IndexOf(n2));
  for (int i = 1; i <= 5; ++i) {
    EXPECT_EQ(group, model()->GetTabGroupForTab(i)) << i;
  }
}

// Multi-selection reduces to the singular active tab.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabMultiSelectionUsesActive) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(3);
  model()->SelectTabAt(1);  // adds to the selection AND activates [1]
  ASSERT_EQ(1, model()->active_index());
  ASSERT_TRUE(model()->IsTabSelected(1));
  ASSERT_TRUE(model()->IsTabSelected(3));
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 2, group);
}

// A pinned active tab: TabStripModel::ConstrainInsertionIndex degrades the
// requested active+1 to the first unpinned slot; the new tab is not pinned
// and (left neighbour pinned/ungrouped) not grouped.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabPinnedActiveDegradesToFirstUnpinned) {
  const TabGroupId group = SetUpT4();
  ASSERT_EQ(0, model()->SetTabPinned(0, true));
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(0);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 1, std::nullopt);
  EXPECT_FALSE(model()->IsTabPinned(1));
  EXPECT_EQ(1, model()->IndexOfFirstNonPinnedTab());
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
}

// A split active tab: InsertionBreaksSplitContiguity pushes the requested
// active+1 past the split; the new tab is not part of it.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabSplitActiveLandsAfterSplit) {
  SetUpU4();
  model()->ActivateTabAt(1);
  const split_tabs::SplitTabId split =
      model()->AddToNewSplit({2}, split_tabs::SplitTabVisualData(),
                             split_tabs::SplitTabCreatedSource::kToolbarButton);
  ASSERT_EQ(split, model()->GetSplitForTab(1));
  ASSERT_EQ(split, model()->GetSplitForTab(2));
  ASSERT_EQ(1, model()->active_index());
  SetMode(NewTabPosition::kAfterActiveTab);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  EXPECT_EQ(3, IndexOf(n));
  EXPECT_EQ(n, model()->GetActiveWebContents());
  EXPECT_EQ(std::nullopt, model()->GetSplitForTab(3));
  EXPECT_EQ(split, model()->GetSplitForTab(1));
  EXPECT_EQ(split, model()->GetSplitForTab(2));
}

// A collapsed group right after the ungrouped active tab: the new tab is
// requested with NO group, the left neighbour is ungrouped, so it does not
// join (or expand) the collapsed group.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       AfterActiveTabCollapsedSiblingGroupStaysUngrouped) {
  const TabGroupId group = SetUpT4();
  model()->ActivateTabAt(0);
  model()->ChangeTabGroupVisuals(
      group,
      tab_groups::TabGroupVisualData(u"G", tab_groups::TabGroupColorId::kGrey,
                                     /*is_collapsed=*/true));
  ASSERT_TRUE(model()->IsGroupCollapsed(group));
  SetMode(NewTabPosition::kAfterActiveTab);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 1, std::nullopt);
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_TRUE(model()->IsGroupCollapsed(group));
}

// ---- opener / close-selection (verified upstream facts, not changed) -------
// Origin is [1] — mid-group, NOT the group's last tab — so "origin" and "a
// right neighbour" can never alias.

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       CloseImmediatelyReturnsToOriginEndOfStrip) {
  SetUpT4();
  content::WebContents* origin = Contents(1);
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  CloseAndWait(n);
  EXPECT_EQ(origin, model()->GetActiveWebContents());
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       CloseImmediatelyReturnsToOriginEndOfActiveGroup) {
  SetUpT4();
  content::WebContents* origin = Contents(1);
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  CloseAndWait(n);
  EXPECT_EQ(origin, model()->GetActiveWebContents());
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       CloseImmediatelyReturnsToOriginAfterActiveTab) {
  SetUpT4();
  content::WebContents* origin = Contents(1);
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  CloseAndWait(n);
  EXPECT_EQ(origin, model()->GetActiveWebContents());
}

// A non-end new tab that receives a typed URL forgets openers strip-wide
// (TabNavigating: forgiven only when IsNewTabAtEndOfTabStrip), so closing it
// afterwards selects a neighbour — never asserted as a specific index.
IN_PROC_BROWSER_TEST_F(
    RoamuxNewTabPositionTest,
    CloseAfterTypedNavigationSelectsNeighbourAfterActiveTab) {
  SetUpT4();
  content::WebContents* origin = Contents(1);
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_LT(IndexOf(n), model()->count() - 1);  // not literally last
  TypedNavigateActiveTab();
  CloseAndWait(n);
  EXPECT_NE(origin, model()->GetActiveWebContents());
}

IN_PROC_BROWSER_TEST_F(
    RoamuxNewTabPositionTest,
    CloseAfterTypedNavigationSelectsNeighbourEndOfActiveGroupNotLast) {
  SetUpT4();
  content::WebContents* origin = Contents(1);
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_LT(IndexOf(n), model()->count() - 1);  // group end != strip end
  TypedNavigateActiveTab();
  CloseAndWait(n);
  EXPECT_NE(origin, model()->GetActiveWebContents());
}

// Control: a literally-last new tab IS forgiven its first typed navigation,
// so the opener survives and closing returns to the origin.
IN_PROC_BROWSER_TEST_F(
    RoamuxNewTabPositionTest,
    CloseAfterTypedNavigationReturnsToOriginWhenNewTabIsLast) {
  SetUpTG();
  content::WebContents* origin = Contents(2);
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(2);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(IndexOf(n), model()->count() - 1);  // literally last
  TypedNavigateActiveTab();
  CloseAndWait(n);
  EXPECT_EQ(origin, model()->GetActiveWebContents());
}

// ---- flag precedence --------------------------------------------------------

// kNewTabPosition OFF: the pref is never read; the stock patch-0067 path.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionFlagOffTest, PrefIgnoredStockPath) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 3, group);
}

// end_of_active_group keeps honouring an explicit upstream opt-out.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionUpstreamOffTest,
                       EndOfActiveGroupHonoursUpstreamOptOut) {
  SetUpT4();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 4, std::nullopt);
}

// after_active_tab ignores the upstream flag: an adjacent in-group tab cannot
// be ungrouped.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionUpstreamOffTest,
                       AfterActiveTabIgnoresUpstreamOptOut) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaCommand();
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 2, group);
}

// ---- the NewTabTypes gate ---------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest, NewTabButtonRouteHonoursMode) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaType(NewTabTypes::kNewTabButton);
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 2, group);
}

// kNoUserAction (the last-grouped-tab-close safety tab, web-app reparenting)
// stays on the stock path even with the feature on.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest, NoUserActionRouteStaysStock) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(1);
  content::WebContents* n = NewTabViaType(NewTabTypes::kNoUserAction);
  ASSERT_EQ(5, model()->count());
  ExpectPlacement(n, 3, group);
}

// ---- accepted affected consumers (frozen analysis, risk 7) ------------------

// CreateNewTabGroup = NewTab() + group-the-active-tab; AddToNewGroupImpl
// forms the new group where the new tab landed.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       CreateNewTabGroupFormsAdjacentUnderAfterActiveTab) {
  const TabGroupId group = SetUpT4();
  SetMode(NewTabPosition::kAfterActiveTab);
  model()->ActivateTabAt(0);
  chrome::ExecuteCommand(browser(), IDC_CREATE_NEW_TAB_GROUP);
  ASSERT_EQ(5, model()->count());
  content::WebContents* n = model()->GetActiveWebContents();
  EXPECT_EQ(1, IndexOf(n));
  const std::optional<TabGroupId> new_group = model()->GetTabGroupForTab(1);
  ASSERT_TRUE(new_group.has_value());
  EXPECT_NE(group, new_group.value());
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_EQ(group, model()->GetTabGroupForTab(3));
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(4));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       CreateNewTabGroupFormsAtEndUnderEndOfStrip) {
  const TabGroupId group = SetUpT4();
  content::WebContents* trailing = Contents(3);
  SetMode(NewTabPosition::kEndOfStrip);
  model()->ActivateTabAt(1);
  chrome::ExecuteCommand(browser(), IDC_CREATE_NEW_TAB_GROUP);
  ASSERT_EQ(5, model()->count());
  content::WebContents* n = model()->GetActiveWebContents();
  EXPECT_EQ(4, IndexOf(n));
  const std::optional<TabGroupId> new_group = model()->GetTabGroupForTab(4);
  ASSERT_TRUE(new_group.has_value());
  EXPECT_NE(group, new_group.value());
  EXPECT_EQ(group, model()->GetTabGroupForTab(1));
  EXPECT_EQ(group, model()->GetTabGroupForTab(2));
  EXPECT_EQ(3, IndexOf(trailing));
  EXPECT_EQ(std::nullopt, model()->GetTabGroupForTab(3));
}

// The behavioural core of web_app_launch_utils.cc's survival tab (an
// automatic kNewTabCommand caller on a one-tab strip): under end_of_strip
// the survival tab is ungrouped; under the other modes it joins the group.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       SurvivalTabOnGroupedSingleTabStripEndOfStrip) {
  const TabGroupId group = SetUpT1G();
  SetMode(NewTabPosition::kEndOfStrip);
  content::WebContents* n = NewTabViaType(NewTabTypes::kNewTabCommand);
  ASSERT_EQ(2, model()->count());
  ExpectPlacement(n, 1, std::nullopt);
  EXPECT_EQ(group, model()->GetTabGroupForTab(0));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionTest,
                       SurvivalTabOnGroupedSingleTabStripJoinsGroup) {
  const TabGroupId group = SetUpT1G();
  SetMode(NewTabPosition::kEndOfActiveGroup);
  content::WebContents* n1 = NewTabViaType(NewTabTypes::kNewTabCommand);
  ASSERT_EQ(2, model()->count());
  ExpectPlacement(n1, 1, group);
  CloseAndWait(n1);
  ASSERT_EQ(1, model()->count());
  model()->ActivateTabAt(0);
  SetMode(NewTabPosition::kAfterActiveTab);
  content::WebContents* n2 = NewTabViaType(NewTabTypes::kNewTabCommand);
  ASSERT_EQ(2, model()->count());
  ExpectPlacement(n2, 1, group);
}

// ---- vertical strip scroll sanity (verify only — no scroll code is added) ---

// Depth-first collection of the strip's VerticalTabViews. While tabs are only
// ever APPENDED (all unpinned, ungrouped) the children order is the model
// order, so the i-th collected view is tab i; after a mid-strip insertion the
// new view is appended to the children list, so post-insertion lookups go
// through IsActive() instead (FindActiveVerticalTabView).
void CollectVerticalTabViews(views::View* root,
                             std::vector<VerticalTabView*>* out) {
  if (auto* tab_view = views::AsViewClass<VerticalTabView>(root)) {
    out->push_back(tab_view);
    return;
  }
  for (views::View* child : root->children()) {
    CollectVerticalTabViews(child, out);
  }
}

VerticalTabView* FindActiveVerticalTabView(views::View* root) {
  if (auto* tab_view = views::AsViewClass<VerticalTabView>(root)) {
    return tab_view->IsActive() ? tab_view : nullptr;
  }
  for (views::View* child : root->children()) {
    if (VerticalTabView* hit = FindActiveVerticalTabView(child)) {
      return hit;
    }
  }
  return nullptr;
}

views::View* FindViewByClassName(views::View* root, std::string_view name) {
  if (root->GetClassName() == name) {
    return root;
  }
  for (views::View* child : root->children()) {
    if (views::View* hit = FindViewByClassName(child, name)) {
      return hit;
    }
  }
  return nullptr;
}

class RoamuxNewTabPositionVerticalScrollTest : public test::RoamuxBrowserTest {
 public:
  RoamuxNewTabPositionVerticalScrollTest() {
    features_.InitWithFeatures(
        {features::kNewTabPosition, ::features::kNewTabAddsToActiveGroup,
         features::kTabStripPosition},
        {});
  }

 protected:
  BrowserView* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }
  void Layout() {
    base::RunLoop().RunUntilIdle();
    browser_view()->DeprecatedLayoutImmediately();
    base::RunLoop().RunUntilIdle();
  }
  // A tab view's bounds in the unpinned scroll view's CONTENTS space.
  gfx::Rect BoundsInScrollContents(views::View* tab_view,
                                   views::ScrollView* scroll_view) {
    return views::View::ConvertRectToTarget(tab_view, scroll_view->contents(),
                                            tab_view->GetLocalBounds());
  }
  std::vector<VerticalTabView*> TabViews() {
    std::vector<VerticalTabView*> views;
    CollectVerticalTabViews(browser_view(), &views);
    return views;
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionVerticalScrollTest,
                       NewTabAfterActiveIsFullyVisible) {
  TabStripModel* const model = browser()->tab_strip_model();
  PrefService* const prefs = browser()->profile()->GetPrefs();
  SetTabStripPlacement(prefs, TabStripPlacement::kLeft);
  Layout();
  auto* strip = views::AsViewClass<VerticalTabStripView>(
      FindViewByClassName(browser_view(), "VerticalTabStripView"));
  ASSERT_NE(nullptr, strip);
  views::ScrollView* const scroll_view =
      strip->unpinned_tabs_scroll_view_for_testing();
  ASSERT_NE(nullptr, scroll_view);

  // Overflow the unpinned scroll view by at least three rows, however tall
  // the test window is.
  for (int added = 0; added < 200; ++added) {
    chrome::AddTabAt(browser(), GURL(url::kAboutBlankURL), /*idx=*/-1,
                     /*foreground=*/true);
    Layout();
    const std::vector<VerticalTabView*> views = TabViews();
    if (views.size() >= 4 && scroll_view->contents()->bounds().height() >=
                                 scroll_view->GetVisibleRect().height() +
                                     3 * views.back()->height()) {
      break;
    }
  }
  model->ActivateTabAt(0);
  Layout();
  const std::vector<VerticalTabView*> views = TabViews();
  ASSERT_EQ(static_cast<size_t>(model->count()), views.size());
  const gfx::Rect viewport = scroll_view->GetVisibleRect();
  ASSERT_LT(viewport.height(), scroll_view->contents()->bounds().height())
      << "the strip must overflow: viewport " << viewport.ToString()
      << " contents " << scroll_view->contents()->bounds().ToString();
  // The last row fully visible at scroll offset 0.
  int last_visible = -1;
  for (size_t i = 0; i < views.size(); ++i) {
    if (viewport.Contains(BoundsInScrollContents(views[i], scroll_view))) {
      last_visible = static_cast<int>(i);
    }
  }
  ASSERT_GT(last_visible, 0) << "viewport " << viewport.ToString();
  ASSERT_LT(last_visible, model->count() - 2)
      << "viewport " << viewport.ToString() << " rows " << views.size();
  model->ActivateTabAt(last_visible);
  Layout();
  ASSERT_TRUE(views[last_visible]->IsActive());
  ASSERT_TRUE(scroll_view->GetVisibleRect().Contains(
      BoundsInScrollContents(views[last_visible], scroll_view)))
      << "precondition: the chosen row must be fully visible before Cmd+T";

  SetNewTabPosition(prefs, NewTabPosition::kAfterActiveTab);
  chrome::ExecuteCommand(browser(), IDC_NEW_TAB);
  ASSERT_EQ(last_visible + 1, model->active_index());
  Layout();
  ASSERT_EQ(static_cast<size_t>(model->count()), TabViews().size());
  VerticalTabView* const new_tab_view =
      FindActiveVerticalTabView(browser_view());
  ASSERT_NE(nullptr, new_tab_view);
  EXPECT_NE(new_tab_view, views[last_visible]);
  EXPECT_TRUE(scroll_view->GetVisibleRect().Contains(
      BoundsInScrollContents(new_tab_view, scroll_view)))
      << "the new active tab (inserted just past the viewport) must have been "
         "scrolled fully into view by the existing vertical-strip logic; "
         "viewport "
      << scroll_view->GetVisibleRect().ToString() << " tab "
      << BoundsInScrollContents(new_tab_view, scroll_view).ToString();
}

}  // namespace
}  // namespace roamux
