// SPDX-License-Identifier: Apache-2.0
// roam-256: an upstream `vertical_tabs.enabled` write must not fire the
// mode-change pipeline while the roamux placement is the sole authority.
//
// NotifyModeChanged is an EDGE signal and
// BrowserView::OnVerticalTabStripModeChanged's vertical branch is not
// idempotent — HorizontalTabStripRegionView::ResetTabStrip has no
// `tab_strip_set_` guard, unlike its InitializeTabStrip counterpart and both
// VerticalTabStripRegionView counterparts. So with placement Left/Right (the
// strip already re-parented into the vertical region) a redundant edge tears
// down a strip that is no longer the horizontal region's, and
// views::View::RemoveChildViewT segfaults. At kTop the same redundant edge
// lands in the idempotent branch and is silently absorbed — which is why the
// crash needs BOTH the flag and a vertical placement.
//
// (TDD: written RED before patch 0063 — Left/Right crash, kTop fails on the
// notification count, and the two control cases pass pre-patch.)

#include <string_view>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/metrics/user_action_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/common/tab_strip_placement.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/views/view.h"

namespace roamux {
namespace {

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

// Shared body: settle `placement`, assert the expected region is the visible
// one, then write the upstream pref and assert NOTHING moved and no
// mode-change notification was emitted.
class ModeNotifyTestBase : public roamux::test::RoamuxBrowserTest {
 protected:
  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  BrowserView* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  ::tabs::VerticalTabStripStateController* controller() {
    return ::tabs::VerticalTabStripStateController::From(browser());
  }

  views::View* vertical_region() {
    return FindViewByClassName(browser_view(), "VerticalTabStripRegionView");
  }

  views::View* horizontal_region() {
    return FindViewByClassName(browser_view(), "HorizontalTabStripRegionView");
  }

  void SettleLayout() {
    base::RunLoop().RunUntilIdle();
    browser_view()->DeprecatedLayoutImmediately();
  }

  void SetPlacementAndSettle(TabStripPlacement placement) {
    SetTabStripPlacement(prefs(), placement);
    SettleLayout();
  }

  base::test::ScopedFeatureList features_;
};

// The shipped default configuration: the roamux flag on, nothing else.
class RoamuxVerticalTabsModeNotifyTest : public ModeNotifyTestBase {
 public:
  RoamuxVerticalTabsModeNotifyTest() {
    features_.InitWithFeatures({features::kTabStripPosition}, {});
  }
};

// The upstream configuration: roamux flag OFF and upstream vertical tabs ON.
// Enabling ::tabs::kVerticalTabs is load-bearing — with it off the upstream
// display predicate is false regardless of the pref and the case would assert
// vacuously (same trap patch 0062's inventory row records).
class RoamuxVerticalTabsModeNotifyFlagOffTest : public ModeNotifyTestBase {
 public:
  RoamuxVerticalTabsModeNotifyFlagOffTest() {
    features_.InitWithFeatures(
        /*enabled_features=*/{::tabs::kVerticalTabs},
        /*disabled_features=*/{features::kTabStripPosition});
  }
};

// Case 1 — the crash repro. Placement Left, fully settled, then the exact write
// BrowserCommandHandler::EnableVerticalTabs() performs on a live profile.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsModeNotifyTest,
                       LeftPlacementPrefWriteIsSuppressed) {
  SetPlacementAndSettle(TabStripPlacement::kLeft);
  ASSERT_NE(nullptr, controller());
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  ASSERT_TRUE(vertical->GetVisible()) << "placement Left must show the strip";

  int mode_changes = 0;
  auto subscription = controller()->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  SettleLayout();

  EXPECT_EQ(0, mode_changes)
      << "the upstream pref cannot change the effective display under the "
         "roam-182 sole-authority contract, so it must not fire the edge";
  EXPECT_TRUE(vertical->GetVisible()) << "the strip must not have been reset";
}

// Case 2 — the same, docked right. Closes the kRight gap the roam-256
// investigation left open.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsModeNotifyTest,
                       RightPlacementPrefWriteIsSuppressed) {
  SetPlacementAndSettle(TabStripPlacement::kRight);
  ASSERT_NE(nullptr, controller());
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  ASSERT_TRUE(vertical->GetVisible()) << "placement Right must show the strip";

  int mode_changes = 0;
  auto subscription = controller()->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  SettleLayout();

  EXPECT_EQ(0, mode_changes);
  EXPECT_TRUE(vertical->GetVisible()) << "the strip must not have been reset";
}

// Case 3 — kTop. The redundant edge is absorbed here rather than fatal, so this
// case is what proves the notification itself is suppressed rather than the
// crash merely being dodged. It also pins the pre-guard side effects, which run
// ahead of the enable-state-lock early return and must survive untouched.
IN_PROC_BROWSER_TEST_F(
    RoamuxVerticalTabsModeNotifyTest,
    TopPlacementPrefWriteIsSuppressedAndPreservesFirstTimeSideEffects) {
  SetPlacementAndSettle(TabStripPlacement::kTop);
  ASSERT_NE(nullptr, controller());
  views::View* horizontal = horizontal_region();
  ASSERT_NE(nullptr, horizontal);
  ASSERT_TRUE(horizontal->GetVisible()) << "placement Top stays horizontal";
  ASSERT_FALSE(prefs()->GetBoolean(::prefs::kVerticalTabsEnabledFirstTime));

  base::UserActionTester actions;
  int mode_changes = 0;
  auto subscription = controller()->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  SettleLayout();

  EXPECT_EQ(0, mode_changes)
      << "same-mode notification must be suppressed even where it is harmless";
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_TRUE(prefs()->GetBoolean(::prefs::kVerticalTabsEnabledFirstTime))
      << "the first-time pref write precedes the guard and must survive it";
  EXPECT_EQ(1, actions.GetActionCount("VerticalTabs_EnabledFirstTime"))
      << "the first-time user action must still be recorded";
}

// Case 4 — over-suppression guard. A real placement flip must still drive the
// swap exactly once; a guard that also swallowed this would break the feature.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsModeNotifyTest,
                       PlacementFlipStillNotifiesAndSwaps) {
  SetPlacementAndSettle(TabStripPlacement::kTop);
  ASSERT_NE(nullptr, controller());
  ASSERT_FALSE(controller()->ShouldDisplayVerticalTabs());

  int mode_changes = 0;
  auto subscription = controller()->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  SetPlacementAndSettle(TabStripPlacement::kLeft);

  EXPECT_EQ(1, mode_changes) << "a real display flip must notify exactly once";
  EXPECT_TRUE(controller()->ShouldDisplayVerticalTabs());
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
}

// Case 5 — flag-off parity. With the roamux flag off the upstream pref IS the
// authority, so upstream's unconditional notify must remain byte-identical.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalTabsModeNotifyFlagOffTest,
                       FlagOffPrefWriteStillNotifiesAndSwaps) {
  SettleLayout();
  ASSERT_NE(nullptr, controller());
  ASSERT_FALSE(controller()->ShouldDisplayVerticalTabs());

  int mode_changes = 0;
  auto subscription = controller()->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  SettleLayout();

  EXPECT_EQ(1, mode_changes)
      << "flag off: the upstream pref is authoritative and must still notify";
  EXPECT_TRUE(controller()->ShouldDisplayVerticalTabs());
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
}

}  // namespace
}  // namespace roamux
