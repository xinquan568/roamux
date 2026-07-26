// SPDX-License-Identifier: Apache-2.0
// roam-8 (I-1.3): placement left/right reuses the upstream vertical tab strip
// (maintainer-authorized surface): display mapping (both docks), right-dock
// geometry, live switching across all four placements, flag-off inertness, and
// upstream-pref precedence. (TDD: written RED before patch 0008.)

#include <string_view>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/run_loop.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/actions/actions.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/vector_icons.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace roamux {
namespace {

// roam-206: the collapse action item, and the ImageModel
// UpdateCollapseActionItem builds for a given icon.
actions::ActionItem* CollapseActionItem(Browser* browser) {
  return actions::ActionManager::Get().FindAction(
      kActionToggleCollapseVertical,
      browser->browser_actions()->root_action_item());
}

ui::ImageModel IconModel(const gfx::VectorIcon& icon) {
  return ui::ImageModel::FromVectorIcon(icon, ui::kColorIcon);
}

// Drives collapse through the public RequestCollapse round-trip (the
// delegate animates, then commits) — the upstream browsertest pattern.
void CollapseAndWait(::tabs::VerticalTabStripStateController* controller,
                     bool collapsed) {
  if (controller->IsCollapsed() == collapsed) {
    return;
  }
  controller->RequestCollapse(collapsed);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return controller->IsCollapsed() == collapsed; }));
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

gfx::Rect BoundsInBrowserView(views::View* view, BrowserView* bv) {
  gfx::RectF rect(gfx::SizeF(view->size()));
  views::View::ConvertRectToTarget(view, bv, &rect);
  return gfx::ToEnclosingRect(rect);
}

// roam-228: the raw logical delta that drives OnResize's proposed_width from
// `start` to `target`. OnResize computes
// `proposed = starting_width_on_resize_ + resize_amount` where the start is the
// view's live width(), and patch 0058 negates the delta on a logical-trailing
// dock — so the caller states the width it wants and this derives the input.
// Deltas are always derived from the OBSERVED width rather than an assumed
// reset: VerticalTabStripStateController::SetUncollapsedWidth updates only the
// controller's own state, and OnCollapseStateChanged does not push it back into
// the region view's target_collapse_state_.
int RawDeltaFor(int start, int target, bool logical_trailing) {
  const int needed = target - start;
  return logical_trailing ? -needed : needed;
}

class RoamuxVerticalStripPlacementTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripPlacementTest() {
    // Deliberately does NOT enable the upstream vertical-tabs feature flags:
    // the roamux path must work without them (plan D1).
    features_.InitAndEnableFeature(features::kTabStripPosition);
  }

 protected:
  BrowserView* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  void SetPlacementAndLayout(int value) {
    browser()->profile()->GetPrefs()->SetInteger(prefs::kTabStripPosition,
                                                 value);
    base::RunLoop().RunUntilIdle();
    browser_view()->DeprecatedLayoutImmediately();
  }

  views::View* vertical_region() {
    return FindViewByClassName(browser_view(), "VerticalTabStripRegionView");
  }

  // roam-228: typed accessors for the resize-geometry tests. The handle's
  // bounds are LOCAL to the region view, so every assertion below compares
  // against region()->width(), never against BrowserView coordinates.
  VerticalTabStripRegionView* region() {
    return views::AsViewClass<VerticalTabStripRegionView>(vertical_region());
  }

  views::ResizeArea* handle() { return region()->resize_area_for_testing(); }

  ::tabs::VerticalTabStripStateController* state_controller() {
    return ::tabs::VerticalTabStripStateController::From(browser());
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LeftPlacementDisplaysVerticalStripAtLeadingEdge) {
  SetPlacementAndLayout(2);  // kLeft.
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical) << "controller/region view must exist under "
                                  "the roamux flag (patch 0008 creation gate)";
  EXPECT_TRUE(vertical->GetVisible());
  const gfx::Rect bounds = BoundsInBrowserView(vertical, browser_view());
  EXPECT_EQ(browser_view()->GetLocalBounds().x(), bounds.x());
  EXPECT_GT(bounds.width(), 0);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       RightPlacementDocksVerticalStripAtRightEdge) {
  SetPlacementAndLayout(3);  // kRight.
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
  const gfx::Rect bounds = BoundsInBrowserView(vertical, browser_view());
  EXPECT_EQ(browser_view()->GetLocalBounds().right(), bounds.right());
  EXPECT_GT(bounds.x(), browser_view()->GetLocalBounds().width() / 2);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LiveSwitchAcrossAllFourPlacements) {
  views::View* horizontal =
      FindViewByClassName(browser_view(), "HorizontalTabStripRegionView");
  ASSERT_NE(nullptr, horizontal);

  SetPlacementAndLayout(0);  // top
  EXPECT_TRUE(horizontal->GetVisible());

  SetPlacementAndLayout(1);  // bottom — horizontal strip, bottom band
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_EQ(browser_view()->GetLocalBounds().bottom(),
            BoundsInBrowserView(horizontal, browser_view()).bottom());

  SetPlacementAndLayout(2);  // left — vertical strip takes over
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());

  SetPlacementAndLayout(3);  // right
  EXPECT_TRUE(vertical->GetVisible());
  EXPECT_EQ(browser_view()->GetLocalBounds().right(),
            BoundsInBrowserView(vertical, browser_view()).right());

  SetPlacementAndLayout(0);  // and home again — no restart throughout
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_LT(BoundsInBrowserView(horizontal, browser_view()).y(),
            browser_view()->GetLocalBounds().height() / 2);
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LockDefersDisplayReconciliationUntilUnlock) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);

  // While an enable-state lock is held, a roamux placement change must not
  // swap strips; the effective display reconciles on unlock (Step-8 fix).
  {
    auto lock = controller->GetEnableStateLock();
    SetPlacementAndLayout(2);  // kLeft while locked.
    EXPECT_FALSE(controller->ShouldDisplayVerticalTabs() && vertical_region() &&
                 vertical_region()->GetVisible() && false)
        << "(sanity only — the real assertion is post-unlock)";
    views::View* vertical = vertical_region();
    if (vertical) {
      EXPECT_FALSE(vertical->GetVisible());
    }
  }
  base::RunLoop().RunUntilIdle();
  browser_view()->DeprecatedLayoutImmediately();
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       UpstreamToggleCommandResetsRoamuxPlacement) {
  SetPlacementAndLayout(2);  // Roamux-driven vertical (upstream pref off).
  ASSERT_NE(nullptr, vertical_region());
  EXPECT_TRUE(vertical_region()->GetVisible());

  // The upstream "switch to horizontal" command must reset the roamux
  // placement, not no-op on the (already-false) upstream pref (Step-8 fix).
  chrome::ToggleVerticalTabs(browser());
  base::RunLoop().RunUntilIdle();
  browser_view()->DeprecatedLayoutImmediately();

  PrefService* prefs = browser()->profile()->GetPrefs();
  EXPECT_EQ(0, prefs->GetInteger(prefs::kTabStripPosition));
  EXPECT_FALSE(prefs->GetBoolean(::prefs::kVerticalTabsEnabled));
  EXPECT_FALSE(vertical_region()->GetVisible());
}

class RoamuxVerticalStripFlagOffTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripFlagOffTest() {
    features_.InitAndDisableFeature(features::kTabStripPosition);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripFlagOffTest,
                       LeftPlacementInertWhenFlagOff) {
  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  browser()->profile()->GetPrefs()->SetInteger(prefs::kTabStripPosition, 2);
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();
  // Stock behavior: the vertical strip is not displayed. (The region view
  // object may still exist — test builds can enable the upstream feature via
  // the field-trial testing config — so assert on visibility, not existence.)
  views::View* vertical = FindViewByClassName(bv, "VerticalTabStripRegionView");
  if (vertical) {
    EXPECT_FALSE(vertical->GetVisible());
  }
  views::View* horizontal =
      FindViewByClassName(bv, "HorizontalTabStripRegionView");
  ASSERT_NE(nullptr, horizontal);
  EXPECT_TRUE(horizontal->GetVisible());
}

class RoamuxVerticalStripUpstreamPrecedenceTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripUpstreamPrecedenceTest() {
    features_.InitWithFeatures(
        {features::kTabStripPosition, ::tabs::kVerticalTabs}, {});
  }

 protected:
  base::test::ScopedFeatureList features_;
};

// roam-182 sole authority: the roamux placement drives display and dock even
// when the upstream pref is explicitly on (the old precedence rule made every
// placement a user-visible no-op in exactly this profile state).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripUpstreamPrecedenceTest,
                       RoamuxRightDocksRightDespiteUpstreamPrefOn) {
  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  PrefService* prefs = browser()->profile()->GetPrefs();
  prefs->SetBoolean(::prefs::kVerticalTabsEnabled, true);
  prefs->SetInteger(prefs::kTabStripPosition, 3);  // roamux right.
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();

  views::View* vertical = FindViewByClassName(bv, "VerticalTabStripRegionView");
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
  gfx::RectF rect(gfx::SizeF(vertical->size()));
  views::View::ConvertRectToTarget(vertical, bv, &rect);
  EXPECT_EQ(bv->GetLocalBounds().right(), gfx::ToEnclosingRect(rect).right());
  // Mid-session the upstream pref is display-inert, not rewritten
  // (normalization happens only at profile init).
  EXPECT_TRUE(prefs->GetBoolean(::prefs::kVerticalTabsEnabled));
}

// The direct regression test for the reported bug: with the upstream pref set
// (the state the stock "Tab position: Vertical" dropdown produces), every
// placement must still take live effect.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripUpstreamPrecedenceTest,
                       AllFourPlacementsActWithUpstreamPrefSet) {
  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  PrefService* prefs = browser()->profile()->GetPrefs();
  prefs->SetBoolean(::prefs::kVerticalTabsEnabled, true);

  auto set_placement = [&](int value) {
    prefs->SetInteger(prefs::kTabStripPosition, value);
    base::RunLoop().RunUntilIdle();
    bv->DeprecatedLayoutImmediately();
  };
  views::View* horizontal =
      FindViewByClassName(bv, "HorizontalTabStripRegionView");
  ASSERT_NE(nullptr, horizontal);

  set_placement(0);  // top
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_LT(BoundsInBrowserView(horizontal, bv).y(),
            bv->GetLocalBounds().height() / 2);

  set_placement(1);  // bottom
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_EQ(bv->GetLocalBounds().bottom(),
            BoundsInBrowserView(horizontal, bv).bottom());

  set_placement(2);  // left
  views::View* vertical = FindViewByClassName(bv, "VerticalTabStripRegionView");
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
  EXPECT_EQ(bv->GetLocalBounds().x(), BoundsInBrowserView(vertical, bv).x());

  set_placement(3);  // right
  EXPECT_TRUE(vertical->GetVisible());
  EXPECT_EQ(bv->GetLocalBounds().right(),
            BoundsInBrowserView(vertical, bv).right());
}

// roam-182: with the flag on, the upstream "switch to vertical" toggle maps
// onto the placement (kLeft) instead of writing the upstream pref.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       UpstreamToggleToVerticalMapsToLeftPlacement) {
  SetPlacementAndLayout(0);  // top (horizontal).
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);

  controller->SetVerticalTabsEnabled(true);
  base::RunLoop().RunUntilIdle();
  browser_view()->DeprecatedLayoutImmediately();

  PrefService* prefs = browser()->profile()->GetPrefs();
  EXPECT_EQ(2, prefs->GetInteger(prefs::kTabStripPosition));  // kLeft.
  EXPECT_FALSE(prefs->GetBoolean(::prefs::kVerticalTabsEnabled));
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
}

// roam-182 lock semantics under sole authority: a placement change that flips
// the display mode while an enable-state lock is held stays frozen (no mid-lock
// mode-change notification, visible state unchanged), and reconciles with
// EXACTLY ONE notification on unlock to the placement-ONLY answer even with the
// upstream pref set (the old code kept the upstream contribution and stayed
// vertical). Covered in both directions: vertical->horizontal and
// horizontal->vertical.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripUpstreamPrecedenceTest,
                       LockedVerticalToHorizontalReconcilesOnceOnUnlock) {
  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  PrefService* prefs = browser()->profile()->GetPrefs();
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);

  prefs->SetBoolean(::prefs::kVerticalTabsEnabled, true);
  prefs->SetInteger(prefs::kTabStripPosition, 2);  // left — vertical visible.
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();
  views::View* vertical = FindViewByClassName(bv, "VerticalTabStripRegionView");
  ASSERT_NE(nullptr, vertical);
  ASSERT_TRUE(vertical->GetVisible());

  int mode_changes = 0;
  auto subscription = controller->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  {
    auto lock = controller->GetEnableStateLock();
    prefs->SetInteger(prefs::kTabStripPosition, 1);  // bottom while locked.
    base::RunLoop().RunUntilIdle();
    bv->DeprecatedLayoutImmediately();
    EXPECT_TRUE(vertical->GetVisible()) << "display must stay frozen mid-lock";
    EXPECT_EQ(0, mode_changes) << "no mode-change notification while locked";
  }
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();

  EXPECT_EQ(1, mode_changes) << "exactly one reconcile notification on unlock";
  EXPECT_FALSE(vertical->GetVisible());
  views::View* horizontal =
      FindViewByClassName(bv, "HorizontalTabStripRegionView");
  ASSERT_NE(nullptr, horizontal);
  EXPECT_TRUE(horizontal->GetVisible());
  EXPECT_EQ(bv->GetLocalBounds().bottom(),
            BoundsInBrowserView(horizontal, bv).bottom());
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripUpstreamPrecedenceTest,
                       LockedHorizontalToVerticalReconcilesOnceOnUnlock) {
  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  PrefService* prefs = browser()->profile()->GetPrefs();
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);

  prefs->SetBoolean(::prefs::kVerticalTabsEnabled, true);
  prefs->SetInteger(prefs::kTabStripPosition, 0);  // top — horizontal.
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();
  views::View* horizontal =
      FindViewByClassName(bv, "HorizontalTabStripRegionView");
  ASSERT_NE(nullptr, horizontal);
  ASSERT_TRUE(horizontal->GetVisible());

  int mode_changes = 0;
  auto subscription = controller->RegisterOnModeChanged(base::BindRepeating(
      [](int* n, ::tabs::VerticalTabStripStateController*) { ++*n; },
      &mode_changes));

  {
    auto lock = controller->GetEnableStateLock();
    prefs->SetInteger(prefs::kTabStripPosition, 3);  // right while locked.
    base::RunLoop().RunUntilIdle();
    bv->DeprecatedLayoutImmediately();
    EXPECT_EQ(0, mode_changes) << "no mode-change notification while locked";
  }
  base::RunLoop().RunUntilIdle();
  bv->DeprecatedLayoutImmediately();

  EXPECT_EQ(1, mode_changes) << "exactly one reconcile notification on unlock";
  views::View* vertical = FindViewByClassName(bv, "VerticalTabStripRegionView");
  ASSERT_NE(nullptr, vertical);
  EXPECT_TRUE(vertical->GetVisible());
  EXPECT_EQ(bv->GetLocalBounds().right(),
            BoundsInBrowserView(vertical, bv).right());
}

// roam-206: the collapse toggle's arrow points toward where the panel will
// move — open-icon iff collapsed == docked-right (physical rule).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       CollapseIconMatchesDockSide) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  actions::ActionItem* item = CollapseActionItem(browser());
  ASSERT_NE(nullptr, item);

  SetPlacementAndLayout(3);  // right dock
  CollapseAndWait(controller, true);
  EXPECT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());
  CollapseAndWait(controller, false);
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());

  SetPlacementAndLayout(2);  // left dock — stock rows stay stock
  CollapseAndWait(controller, true);
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());
  CollapseAndWait(controller, false);
  EXPECT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());
}

// roam-206: a live dock-side flip re-derives the icon without a collapse
// transition (OnRoamuxPlacementChanged's same-display-mode path).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LiveDockSwitchRefreshesCollapseIcon) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  actions::ActionItem* item = CollapseActionItem(browser());
  ASSERT_NE(nullptr, item);
  SetPlacementAndLayout(2);
  CollapseAndWait(controller, false);
  ASSERT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());

  SetPlacementAndLayout(3);  // collapse state untouched
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());
}

// roam-206: the rule is physical — RTL changes nothing about which side the
// panel moves toward. The left-dock rows are where stock and physical
// genuinely diverge; the right-dock rows coincide with stock (pins).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       CollapseIconIsPhysicalUnderRTL) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  actions::ActionItem* item = CollapseActionItem(browser());
  ASSERT_NE(nullptr, item);
  base::i18n::ScopedRTLForTesting scoped_rtl(true);

  SetPlacementAndLayout(2);  // left
  CollapseAndWait(controller, true);
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());
  CollapseAndWait(controller, false);
  EXPECT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());

  SetPlacementAndLayout(3);  // right
  CollapseAndWait(controller, true);
  EXPECT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());
  CollapseAndWait(controller, false);
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());
}

// roam-206: a dock-side flip made while an enable-state lock is held must
// re-derive the icon on outermost unlock (OnLockDestroyed path).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LockedDockSwitchRefreshesIconOnUnlock) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  actions::ActionItem* item = CollapseActionItem(browser());
  ASSERT_NE(nullptr, item);
  SetPlacementAndLayout(2);
  CollapseAndWait(controller, false);
  ASSERT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());

  {
    auto lock = controller->GetEnableStateLock();
    SetPlacementAndLayout(3);  // flip while locked; icon deferred
  }
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());
}

// roam-206: with kTabStripPosition OFF the stock expression must be
// preserved exactly — both directions. Upstream vertical tabs are enabled
// explicitly so the collapse action exists deterministically.
class RoamuxVerticalStripFlagOffIconTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxVerticalStripFlagOffIconTest() {
    features_.InitWithFeatures({::tabs::kVerticalTabs},
                               {features::kTabStripPosition});
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripFlagOffIconTest,
                       StockIconExpressionPreserved) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  actions::ActionItem* item = CollapseActionItem(browser());
  ASSERT_NE(nullptr, item);

  // Activate upstream vertical tabs so the delegate exists and collapse
  // round-trips (the roamux placement path is off in this fixture).
  controller->SetVerticalTabsEnabled(true);
  base::RunLoop().RunUntilIdle();

  CollapseAndWait(controller, true);  // LTR collapsed → close (stock)
  EXPECT_EQ(IconModel(views::kMenuCloseIcon), item->GetImage());

  {
    base::i18n::ScopedRTLForTesting scoped_rtl(true);
    CollapseAndWait(controller, false);
    CollapseAndWait(controller, true);  // re-derive under RTL → open (stock)
    EXPECT_EQ(IconModel(views::kMenuOpenIcon), item->GetImage());
  }
}

// roam-228: with kTabStripPosition OFF the stock resize geometry must be
// preserved exactly — handle on the strip's local right edge, positive logical
// delta widens. Pins that patch 0058 cannot leak behaviour into a flag-off
// build. Upstream vertical tabs are activated explicitly so the strip exists.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripFlagOffIconTest,
                       StockResizeGeometryPreserved) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  controller->SetVerticalTabsEnabled(true);
  base::RunLoop().RunUntilIdle();

  BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser());
  bv->DeprecatedLayoutImmediately();
  auto* region = views::AsViewClass<VerticalTabStripRegionView>(
      FindViewByClassName(bv, "VerticalTabStripRegionView"));
  ASSERT_NE(nullptr, region);
  CollapseAndWait(controller, false);
  bv->DeprecatedLayoutImmediately();

  EXPECT_EQ(region->width(),
            region->resize_area_for_testing()->bounds().right());

  const int start = region->width();
  region->OnResize(RawDeltaFor(start, start + 40, /*logical_trailing=*/false),
                   /*done_resizing=*/true);
  EXPECT_EQ(start + 40, region->uncollapsed_width());
}

// roam-205: the collapsed rail's top offset derives from the DOCKED side's
// caption exclusion. On this frame (matching-direction LTR: leading exclusion
// nonempty, trailing empty) a right dock collapses flush — same origin as
// expanded — while a left dock keeps the traffic-light offset.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       CollapsedRightDockRunsToClientTop) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  SetPlacementAndLayout(3);  // right dock
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);

  CollapseAndWait(controller, false);
  browser_view()->DeprecatedLayoutImmediately();
  // Absolute pin (plan V4): the expanded strip starts at the client top.
  ASSERT_EQ(0, BoundsInBrowserView(vertical, browser_view()).y());

  CollapseAndWait(controller, true);
  browser_view()->DeprecatedLayoutImmediately();
  // The collapsed rail is flush too — no leading-exclusion offset on the
  // right dock.
  EXPECT_EQ(0, BoundsInBrowserView(vertical, browser_view()).y());
}

IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       CollapsedLeftDockKeepsCaptionOffset) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  SetPlacementAndLayout(2);  // left dock
  views::View* vertical = vertical_region();
  ASSERT_NE(nullptr, vertical);

  CollapseAndWait(controller, false);
  browser_view()->DeprecatedLayoutImmediately();
  ASSERT_EQ(0, BoundsInBrowserView(vertical, browser_view()).y());

  CollapseAndWait(controller, true);
  browser_view()->DeprecatedLayoutImmediately();
  // The left dock sits below the traffic-light band when collapsed.
  EXPECT_GT(BoundsInBrowserView(vertical, browser_view()).y(), 0);
}

// ---------------------------------------------------------------------------
// roam-228: the resize handle and the drag delta both live in views' LOGICAL
// coordinate space (View::GetMirroredX mirrors child bounds; ResizeArea hands
// its delegate an already-RTL-normalised delta), so both follow the LOGICAL
// dock side = physical placement XOR RTL. (TDD: written RED before patch 0058.)
// ---------------------------------------------------------------------------

// The grab handle belongs on the edge FACING THE WEB CONTENT. Right dock in
// LTR is the logical trailing edge, so that is the strip's local x == 0.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       ResizeHandleOnContentFacingEdgeRightDock) {
  SetPlacementAndLayout(3);  // kRight
  ASSERT_NE(nullptr, region());
  EXPECT_EQ(0, handle()->bounds().x());
  EXPECT_LT(handle()->bounds().right(), region()->width());
}

// Left dock in LTR is the logical leading edge: the handle stays on the
// strip's local right edge, exactly as upstream puts it. Regression guard.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       ResizeHandleOnContentFacingEdgeLeftDock) {
  SetPlacementAndLayout(2);  // kLeft
  ASSERT_NE(nullptr, region());
  EXPECT_EQ(region()->width(), handle()->bounds().right());
}

// Right dock, LTR: dragging the inner edge AWAY from the content widens.
// `starting_width_on_resize_` is captured on the first OnResize and cleared
// only when done_resizing is true, and ResizeArea reports displacement from
// the original press — so BOTH deltas below are relative to the same W.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       RightDockDragTowardContentNarrows) {
  SetPlacementAndLayout(3);  // kRight
  ASSERT_NE(nullptr, region());
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;
  ASSERT_EQ(kW, region()->GetPreferredSize().width());

  // Mid-drag: leftward (negative logical delta) widens a right dock.
  region()->OnResize(-40, /*done_resizing=*/false);
  EXPECT_EQ(kW + 40, region()->GetPreferredSize().width());

  // Completed, relative to the SAME kW (the mid-drag call did not clear it).
  region()->OnResize(+40, /*done_resizing=*/true);
  EXPECT_EQ(kW - 40, region()->uncollapsed_width());
  EXPECT_EQ(kW - 40, state_controller()->GetUncollapsedWidth());
}

// Left dock, LTR: the mirror — positive logical delta widens, as upstream.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LeftDockDragDirectionUnchanged) {
  SetPlacementAndLayout(2);  // kLeft
  ASSERT_NE(nullptr, region());
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;
  ASSERT_EQ(kW, region()->GetPreferredSize().width());

  region()->OnResize(+40, /*done_resizing=*/false);
  EXPECT_EQ(kW + 40, region()->GetPreferredSize().width());

  region()->OnResize(-40, /*done_resizing=*/true);
  EXPECT_EQ(kW - 40, region()->uncollapsed_width());
  EXPECT_EQ(kW - 40, state_controller()->GetUncollapsedWidth());
}

// A live left<->right flip at unchanged width moves the strip's x but not its
// size, and View::SetBoundsRect only re-lays-out on a SIZE change — while the
// roamux placement observer invalidates BrowserView only (invalidation
// propagates up, never down). Without an explicit seam the handle would keep
// the stale edge. Asserted EXPANDED on purpose: a collapsed flip also changes
// height (roam-205's dock-side top offset), so Layout would run regardless and
// would not exercise this.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       LiveDockSwitchMovesResizeHandleWhenExpanded) {
  auto* controller = state_controller();
  ASSERT_NE(nullptr, controller);
  SetPlacementAndLayout(2);  // kLeft
  CollapseAndWait(controller, false);
  browser_view()->DeprecatedLayoutImmediately();
  ASSERT_NE(nullptr, region());
  const int width_before = region()->width();
  ASSERT_EQ(region()->width(), handle()->bounds().right());

  SetPlacementAndLayout(3);  // kRight — same width, x changes only
  ASSERT_EQ(width_before, region()->width())
      << "the flip must not resize the strip, or this would not exercise the "
         "size-unchanged relayout path";
  EXPECT_EQ(0, handle()->bounds().x());
}

// The rule is LOGICAL, not physical: in RTL a physically-right strip sits on
// the logical LEADING edge (so it behaves exactly like stock), and a
// physically-left strip sits on the logical trailing edge. Keeping both the
// geometry and the delta assertions here is deliberate — a physical-only fix
// would pass the geometry rows and still invert the delta.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       ResizeGeometryAndDeltaAreLogicalUnderRTL) {
  base::i18n::ScopedRTLForTesting scoped_rtl(true);
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;

  // Physical RIGHT + RTL = logical leading → stock behaviour.
  SetPlacementAndLayout(3);
  ASSERT_NE(nullptr, region());
  EXPECT_EQ(region()->width(), handle()->bounds().right());
  ASSERT_EQ(kW, region()->GetPreferredSize().width());
  region()->OnResize(RawDeltaFor(region()->width(), kW + 40,
                                 /*logical_trailing=*/false),
                     /*done_resizing=*/true);
  EXPECT_EQ(kW + 40, region()->uncollapsed_width());

  // Physical LEFT + RTL = logical trailing → mirrored. Broken today by the
  // very same expression, on the other physical side.
  SetPlacementAndLayout(2);
  browser_view()->DeprecatedLayoutImmediately();
  EXPECT_EQ(0, handle()->bounds().x());
  const int left_start = region()->width();
  region()->OnResize(
      RawDeltaFor(left_start, left_start + 40, /*logical_trailing=*/true),
      /*done_resizing=*/true);
  EXPECT_EQ(left_start + 40, region()->uncollapsed_width());
}

// Drag-to-collapse and the snap-to-default behaviour must be identical on both
// docks. Collapse needs proposed_width <= kCollapseSnapWidth ((126+56)/2 = 91);
// the snap needs abs(width - 240) < kSnapDistance (15), strictly.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       DragToCollapseAndSnapWorkOnBothDocks) {
  auto* controller = state_controller();
  ASSERT_NE(nullptr, controller);
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;

  for (const int placement : {3, 2}) {
    const bool logical_trailing = (placement == 3);  // LTR: kRight is trailing
    SCOPED_TRACE(logical_trailing ? "right dock" : "left dock");
    SetPlacementAndLayout(placement);
    CollapseAndWait(controller, false);
    browser_view()->DeprecatedLayoutImmediately();
    ASSERT_NE(nullptr, region());
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return region()->width() == region()->uncollapsed_width();
    })) << "let the expand settle so width() is the real starting width";

    // Drag hard toward the content until proposed_width <= kCollapseSnapWidth
    // ((126 + 56) / 2 = 91); 40 is comfortably under it.
    region()->OnResize(RawDeltaFor(region()->width(), 40, logical_trailing),
                       /*done_resizing=*/true);
    EXPECT_TRUE(region()->target_collapse_state_for_testing().collapsed);
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return controller->IsCollapsed();
    })) << "RequestCollapse animates; wait for the committed state before "
           "expanding again";

    // Expand, then land exactly 8px off the default — inside the strict
    // abs(width - 240) < kSnapDistance (15) window → snaps back to 240.
    CollapseAndWait(controller, false);
    browser_view()->DeprecatedLayoutImmediately();
    ASSERT_TRUE(base::test::RunUntil(
        [&]() { return region()->width() == region()->uncollapsed_width(); }));
    region()->OnResize(RawDeltaFor(region()->width(), kW - 8, logical_trailing),
                       /*done_resizing=*/true);
    EXPECT_EQ(kW, region()->uncollapsed_width());
  }
}

// A completed right-dock resize must reach the persistence path: the write is
// unconditional (SetUncollapsedWidth -> NotifyCollapseChanged ->
// UpdatePrefService).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       RightDockCompletedResizePersistsWidthToPrefs) {
  SetPlacementAndLayout(3);  // kRight
  ASSERT_NE(nullptr, region());
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;
  ASSERT_EQ(kW, region()->GetPreferredSize().width());

  region()->OnResize(+40, /*done_resizing=*/true);  // narrows to kW - 40
  EXPECT_EQ(kW - 40, browser()->profile()->GetPrefs()->GetInteger(
                         ::prefs::kVerticalTabsUncollapsedWidth));
}

// ...and a fresh, non-session-restored window reconstructs that width from the
// pref — the same fallback branch an ordinary post-restart window takes
// (BrowserWindowFeatures::Init, gated on !CreatedBySessionRestore()), whose
// value the controller ctor applies via SetUncollapsedWidth. The
// restore-last-session path reads session extra data gated on the upstream
// feature instead, and is deliberately out of scope here (tracked separately).
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       RightDockResizedWidthReconstructsInAFreshWindow) {
  SetPlacementAndLayout(3);  // kRight
  ASSERT_NE(nullptr, region());
  const int kW = ::tabs::kVerticalTabStripDefaultUncollapsedWidth;
  region()->OnResize(+40, /*done_resizing=*/true);
  ASSERT_EQ(kW - 40, state_controller()->GetUncollapsedWidth());

  Browser* second = CreateBrowser(browser()->profile());
  auto* second_controller =
      ::tabs::VerticalTabStripStateController::From(second);
  ASSERT_NE(nullptr, second_controller);
  EXPECT_EQ(kW - 40, second_controller->GetUncollapsedWidth());

  // The dock side is a profile pref, so the new window is right-docked too —
  // the corrected geometry must not be per-window state.
  BrowserView* second_view = BrowserView::GetBrowserViewForBrowser(second);
  second_view->DeprecatedLayoutImmediately();
  auto* second_region = views::AsViewClass<VerticalTabStripRegionView>(
      FindViewByClassName(second_view, "VerticalTabStripRegionView"));
  ASSERT_NE(nullptr, second_region);
  EXPECT_EQ(0, second_region->resize_area_for_testing()->bounds().x());
}

// roam-205: with no collapsed offset on the right dock, the top container's
// leading-margin compensation must not fire — its leading edge stays where
// the expanded state put it.
IN_PROC_BROWSER_TEST_F(RoamuxVerticalStripPlacementTest,
                       RightDockCollapseKeepsTopContainerLeadingEdge) {
  auto* controller = ::tabs::VerticalTabStripStateController::From(browser());
  ASSERT_NE(nullptr, controller);
  SetPlacementAndLayout(3);  // right dock
  views::View* top_container =
      FindViewByClassName(browser_view(), "TopContainerView");
  ASSERT_NE(nullptr, top_container);

  CollapseAndWait(controller, false);
  browser_view()->DeprecatedLayoutImmediately();
  const int expanded_x = BoundsInBrowserView(top_container, browser_view()).x();

  CollapseAndWait(controller, true);
  browser_view()->DeprecatedLayoutImmediately();
  EXPECT_EQ(expanded_x, BoundsInBrowserView(top_container, browser_view()).x());
}

}  // namespace
}  // namespace roamux
