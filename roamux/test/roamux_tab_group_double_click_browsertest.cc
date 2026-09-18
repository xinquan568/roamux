// SPDX-License-Identifier: Apache-2.0
// roam-322: double-click a tab group header to collapse or expand ALL groups —
// end to end in BOTH strips (Roamux placement Top = horizontal TabGroupHeader,
// Left = VerticalTabGroupHeaderView), through the real controllers.
//
// Events are dispatched to the header's own handlers (the upstream
// vertical_tab_group_view_browsertest precedent) with explicit timestamps; a
// double-click is exactly two press/release pairs, the second pair carrying
// ui::EF_IS_DOUBLE_CLICK (what views' repeat classification sets on a native
// second press and its release). Synthetic events never run that
// classification, so tests set the flag themselves.

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/test/metrics/user_action_tester.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/features.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/tab_group_header.h"
#include "chrome/browser/ui/views/tabs/tab_strip.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_group_header_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_group_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_view.h"
#include "components/prefs/pref_service.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "components/tabs/public/tab_group.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace roamux {
namespace {

using tab_groups::TabGroupId;

constexpr int kPlacementTop = 0;
constexpr int kPlacementLeft = 2;
constexpr char kCollapsedAction[] = "TabGroups_TabGroupHeader_Collapsed";
constexpr char kExpandedAction[] = "TabGroups_TabGroupHeader_Expanded";

template <typename T>
T* FindDescendant(views::View* root, const std::function<bool(T*)>& matches) {
  if (!root) {
    return nullptr;
  }
  if (T* v = views::AsViewClass<T>(root); v && matches(v)) {
    return v;
  }
  for (views::View* child : root->children()) {
    if (T* found = FindDescendant<T>(child, matches)) {
      return found;
    }
  }
  return nullptr;
}

class RoamuxTabGroupDoubleClickTestBase : public test::RoamuxBrowserTest {
 protected:
  RoamuxTabGroupDoubleClickTestBase(bool vertical, bool feature_on)
      : vertical_(vertical) {
    std::vector<base::test::FeatureRef> on = {
        features::kTabStripPosition, ::tabs::kVerticalTabs,
        ::features::kTabGroupsCollapseFreezing};
    std::vector<base::test::FeatureRef> off;
    (feature_on ? on : off).push_back(features::kTabGroupHeaderDoubleClickAll);
    features_.InitWithFeatures(on, off);
  }

  void SetUpOnMainThread() override {
    test::RoamuxBrowserTest::SetUpOnMainThread();
    browser()->profile()->GetPrefs()->SetInteger(
        prefs::kTabStripPosition, vertical_ ? kPlacementLeft : kPlacementTop);
    // A NEW window picks the placement up deterministically.
    test_browser_ = CreateBrowser(browser()->profile());
    ASSERT_TRUE(base::test::RunUntil([&]() { return StripReady(); }));
    // Tab 0 (from CreateBrowser) stays ungrouped and active; 6 more tabs make
    // groups A{1,2}, B{3,4}, C{5,6}.
    for (int i = 0; i < 6; ++i) {
      chrome::AddTabAt(test_browser_, GURL("about:blank"), -1,
                       /*foreground=*/false);
    }
    model()->ActivateTabAt(0);
    a_ = model()->AddToNewGroup({1, 2});
    b_ = model()->AddToNewGroup({3, 4});
    c_ = model()->AddToNewGroup({5, 6});
    ASSERT_TRUE(base::test::RunUntil(
        [&]() { return HeaderView(a_) && HeaderView(b_) && HeaderView(c_); }));
    actions_ = std::make_unique<base::UserActionTester>();
  }

  void TearDownOnMainThread() override {
    actions_.reset();
    test::RoamuxBrowserTest::TearDownOnMainThread();
  }

  BrowserView* view() {
    return BrowserView::GetBrowserViewForBrowser(test_browser_);
  }
  TabStripModel* model() { return test_browser_->tab_strip_model(); }

  bool StripReady() {
    if (vertical_) {
      return view()->vertical_tab_strip_region_view_for_testing() != nullptr;
    }
    TabStrip* strip = view()->horizontal_tab_strip_for_testing();
    return strip && strip->GetVisible();
  }

  VerticalTabGroupView* VerticalGroupView(const TabGroupId& id) {
    return FindDescendant<VerticalTabGroupView>(
        view()->vertical_tab_strip_region_view_for_testing(),
        [&](VerticalTabGroupView* g) {
          return g->IsValid() && g->GetTabGroup().id() == id;
        });
  }

  views::View* HeaderView(const TabGroupId& id) {
    if (vertical_) {
      VerticalTabGroupView* g = VerticalGroupView(id);
      return g ? static_cast<views::View*>(g->group_header()) : nullptr;
    }
    TabStrip* strip = view()->horizontal_tab_strip_for_testing();
    return strip ? strip->group_header(id) : nullptr;
  }

  bool Collapsed(const TabGroupId& id) {
    return model()
        ->group_model()
        ->GetTabGroup(id)
        ->visual_data()
        ->is_collapsed();
  }

  // Raw handler calls with explicit time stamps and flags.
  void Press(const TabGroupId& id,
             base::TimeTicks t,
             int extra_flags = 0,
             int button = ui::EF_LEFT_MOUSE_BUTTON) {
    ui::MouseEvent e(ui::EventType::kMousePressed, gfx::Point(4, 4),
                     gfx::Point(4, 4), t, button | extra_flags, button);
    HeaderView(id)->OnMousePressed(e);
  }
  void Release(const TabGroupId& id,
               base::TimeTicks t,
               int extra_flags = 0,
               int button = ui::EF_LEFT_MOUSE_BUTTON) {
    ui::MouseEvent e(ui::EventType::kMouseReleased, gfx::Point(4, 4),
                     gfx::Point(4, 4), t, button | extra_flags, button);
    HeaderView(id)->OnMouseReleased(e);
  }
  // A drag along the strip's axis, well past the views drag threshold.
  void Drag(const TabGroupId& id, base::TimeTicks t) {
    const gfx::Point to = vertical_ ? gfx::Point(4, 40) : gfx::Point(40, 4);
    ui::MouseEvent e(ui::EventType::kMouseDragged, to, to, t,
                     ui::EF_LEFT_MOUSE_BUTTON, ui::EF_LEFT_MOUSE_BUTTON);
    HeaderView(id)->OnMouseDragged(e);
  }
  void Click(const TabGroupId& id, base::TimeTicks t) {
    Press(id, t);
    Release(id, t + base::Milliseconds(40));
  }
  // First pair plain, second pair flagged: one native double-click.
  void DoubleClick(const TabGroupId& id,
                   base::TimeTicks t,
                   base::TimeDelta second_press = base::Milliseconds(150),
                   base::TimeDelta second_release = base::Milliseconds(190)) {
    Click(id, t);
    Press(id, t + second_press, ui::EF_IS_DOUBLE_CLICK);
    Release(id, t + second_release, ui::EF_IS_DOUBLE_CLICK);
  }

  int HeaderActions() {
    return actions_->GetActionCount(kCollapsedAction) +
           actions_->GetActionCount(kExpandedAction);
  }

  void Settle() {
    ASSERT_TRUE(base::test::RunUntil([&]() { return StripReady(); }));
  }

  bool ActiveTabIsUngrouped() {
    return !model()->GetActiveTab()->GetGroup().has_value();
  }

  const bool vertical_;
  base::test::ScopedFeatureList features_;
  raw_ptr<Browser, AcrossTasksDanglingUntriaged> test_browser_ = nullptr;
  std::unique_ptr<base::UserActionTester> actions_;
  TabGroupId a_ = TabGroupId::GenerateNew();
  TabGroupId b_ = TabGroupId::GenerateNew();
  TabGroupId c_ = TabGroupId::GenerateNew();
};

// ---------------------------------------------------------------------------
// Feature ON, both strips.

class RoamuxTabGroupDoubleClickTest : public RoamuxTabGroupDoubleClickTestBase,
                                      public testing::WithParamInterface<bool> {
 public:
  RoamuxTabGroupDoubleClickTest()
      : RoamuxTabGroupDoubleClickTestBase(/*vertical=*/GetParam(),
                                          /*feature_on=*/true) {}
};

INSTANTIATE_TEST_SUITE_P(Strips,
                         RoamuxTabGroupDoubleClickTest,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? std::string("Vertical")
                                             : std::string("Horizontal");
                         });

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest, AllExpandedCollapsesAll) {
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  EXPECT_TRUE(Collapsed(a_));
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_TRUE(Collapsed(c_));
  EXPECT_EQ(HeaderActions(), 1) << "one action per completed gesture";
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest, OneCollapsedExpandsAll) {
  Click(b_, base::TimeTicks::Now());  // B collapsed beforehand
  ASSERT_TRUE(Collapsed(b_));
  const int before = HeaderActions();
  DoubleClick(a_, base::TimeTicks::Now() + base::Seconds(2));
  Settle();
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(c_));
  EXPECT_EQ(HeaderActions() - before, 1);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       DoubleClickOnACollapsedGroupExpandsAll) {
  Click(a_, base::TimeTicks::Now());  // A collapsed beforehand
  DoubleClick(a_, base::TimeTicks::Now() + base::Seconds(2));
  Settle();
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(c_));
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       SingleClickTogglesOnlyThatGroup) {
  Click(b_, base::TimeTicks::Now());
  Settle();
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(c_));
  EXPECT_EQ(HeaderActions(), 1);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       HeldSecondPressStillRunsTheBulkPass) {
  // Second press within 500 ms of the first release; its release 1.2 s later.
  DoubleClick(a_, base::TimeTicks::Now(), base::Milliseconds(200),
              base::Milliseconds(1200));
  Settle();
  EXPECT_TRUE(Collapsed(a_));
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_TRUE(Collapsed(c_));
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       DragAfterTheSecondPressIsNotABulkPass) {
  // The release inherits the press's double-click flag even after a drag;
  // a drag past the threshold must end the gesture.
  const base::TimeTicks t = base::TimeTicks::Now();
  Click(a_, t);
  Press(a_, t + base::Milliseconds(150), ui::EF_IS_DOUBLE_CLICK);
  Drag(a_, t + base::Milliseconds(170));
  Release(a_, t + base::Milliseconds(190), ui::EF_IS_DOUBLE_CLICK);
  Settle();
  EXPECT_FALSE(Collapsed(b_)) << "no bulk pass after a drag";
  EXPECT_FALSE(Collapsed(c_)) << "no bulk pass after a drag";
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       StaleSecondPressIsASingleToggle) {
  DoubleClick(a_, base::TimeTicks::Now(), base::Milliseconds(900),
              base::Milliseconds(940));
  Settle();
  EXPECT_FALSE(Collapsed(a_)) << "two single toggles";
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(c_));
  EXPECT_EQ(HeaderActions(), 2);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       InterveningGroupChangeMakesItASingleToggle) {
  const base::TimeTicks t = base::TimeTicks::Now();
  Click(a_, t);
  // Another group changes between the clicks.
  model()->ChangeTabGroupVisuals(
      c_,
      tab_groups::TabGroupVisualData(u"", tab_groups::TabGroupColorId::kBlue,
                                     /*is_collapsed=*/true));
  Press(a_, t + base::Milliseconds(150), ui::EF_IS_DOUBLE_CLICK);
  Release(a_, t + base::Milliseconds(190), ui::EF_IS_DOUBLE_CLICK);
  Settle();
  EXPECT_FALSE(Collapsed(a_)) << "the second release toggled A back";
  EXPECT_FALSE(Collapsed(b_)) << "no bulk pass";
  EXPECT_EQ(HeaderActions(), 2);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       UnmatchedFlaggedReleaseIsASingleToggle) {
  // No recorded first click (e.g. the first click hit an open editor).
  const base::TimeTicks t = base::TimeTicks::Now();
  Press(a_, t, ui::EF_IS_DOUBLE_CLICK);
  Release(a_, t + base::Milliseconds(40), ui::EF_IS_DOUBLE_CLICK);
  Settle();
  EXPECT_TRUE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_EQ(HeaderActions(), 1);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       ActiveTabInsideCollapsingGroupMovesOut) {
  model()->ActivateTabAt(1);  // inside A
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  EXPECT_TRUE(Collapsed(a_));
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_TRUE(Collapsed(c_));
  EXPECT_TRUE(ActiveTabIsUngrouped());
  EXPECT_EQ(model()->count(), 7) << "an expanded ungrouped tab existed";
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       NoExpandedTabLeftOpensANewUngroupedTab) {
  model()->ActivateTabAt(1);                                  // inside A
  model()->CloseWebContentsAt(0, TabCloseTypes::CLOSE_NONE);  // drop tab 0
  ASSERT_EQ(model()->count(), 6);
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  EXPECT_TRUE(Collapsed(a_));
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_TRUE(Collapsed(c_));
  EXPECT_TRUE(ActiveTabIsUngrouped());
  EXPECT_EQ(model()->count(), 7) << "a new ungrouped tab was opened";
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest, OneGroupWindow) {
  model()->CloseAllTabsInGroup(b_);
  model()->CloseAllTabsInGroup(c_);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return model()->group_model()->ListTabGroups().size() == 1u; }));
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  EXPECT_TRUE(Collapsed(a_)) << "same end state as a single click";
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       KeyboardSpaceAndEnterToggleOneGroup) {
  HeaderView(b_)->OnKeyPressed(
      ui::KeyEvent(ui::EventType::kKeyPressed, ui::VKEY_SPACE, ui::EF_NONE));
  EXPECT_TRUE(Collapsed(b_));
  HeaderView(b_)->OnKeyPressed(
      ui::KeyEvent(ui::EventType::kKeyPressed, ui::VKEY_RETURN, ui::EF_NONE));
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(c_));
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest, TouchTapTogglesOneGroup) {
  ui::GestureEvent tap(4, 4, ui::EF_NONE, base::TimeTicks::Now(),
                       ui::GestureEventDetails(ui::EventType::kGestureTap));
  HeaderView(b_)->OnGestureEvent(&tap);
  EXPECT_TRUE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(c_));
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest, RightClickDoesNotToggle) {
  const base::TimeTicks t = base::TimeTicks::Now();
  Press(b_, t, 0, ui::EF_RIGHT_MOUSE_BUTTON);
  Release(b_, t + base::Milliseconds(40), 0, ui::EF_RIGHT_MOUSE_BUTTON);
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_EQ(HeaderActions(), 0);
}

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickTest,
                       FreezingVotesFollowTheBulkPass) {
  if (!GetParam()) {
    GTEST_SKIP() << "freezing votes are a vertical-strip mechanism";
  }
  auto votes = [&](const TabGroupId& id) {
    int held = 0;
    int total = 0;
    std::function<void(views::View*)> walk = [&](views::View* v) {
      if (auto* t = views::AsViewClass<VerticalTabView>(v)) {
        ++total;
        held += t->HasFreezingVote() ? 1 : 0;
      }
      for (views::View* child : v->children()) {
        walk(child);
      }
    };
    walk(VerticalGroupView(id));
    return std::make_pair(held, total);
  };
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  for (const TabGroupId& id : {a_, b_, c_}) {
    auto [held, total] = votes(id);
    EXPECT_EQ(held, total) << "collapsed by the gesture: every tab frozen";
  }
  DoubleClick(a_, base::TimeTicks::Now() + base::Seconds(2));
  Settle();
  for (const TabGroupId& id : {a_, b_, c_}) {
    EXPECT_FALSE(Collapsed(id));
    EXPECT_EQ(votes(id).first, 0) << "expanded by the gesture: votes released";
  }
}

// ---------------------------------------------------------------------------
// Feature OFF: a double-click is two ordinary single toggles (upstream).

class RoamuxTabGroupDoubleClickFlagOffTest
    : public RoamuxTabGroupDoubleClickTestBase,
      public testing::WithParamInterface<bool> {
 public:
  RoamuxTabGroupDoubleClickFlagOffTest()
      : RoamuxTabGroupDoubleClickTestBase(/*vertical=*/GetParam(),
                                          /*feature_on=*/false) {}
};

INSTANTIATE_TEST_SUITE_P(Strips,
                         RoamuxTabGroupDoubleClickFlagOffTest,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? std::string("Vertical")
                                             : std::string("Horizontal");
                         });

IN_PROC_BROWSER_TEST_P(RoamuxTabGroupDoubleClickFlagOffTest,
                       DoubleClickIsTwoSingleToggles) {
  DoubleClick(a_, base::TimeTicks::Now());
  Settle();
  EXPECT_FALSE(Collapsed(a_));
  EXPECT_FALSE(Collapsed(b_));
  EXPECT_FALSE(Collapsed(c_));
  EXPECT_EQ(HeaderActions(), 2);
}

}  // namespace
}  // namespace roamux
