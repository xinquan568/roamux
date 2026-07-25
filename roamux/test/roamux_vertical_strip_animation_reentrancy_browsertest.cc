// SPDX-License-Identifier: Apache-2.0
// roam-221: re-entrant expand-on-hover start when a collapse animation ends.
// With a kKeepExpanded expand-on-hover lock held (e.g. the roam-214 pin, or
// any upstream producer), the collapse-completed notification synchronously
// re-enters the animation controller: SetCollapsed(true) ->
// OnCollapseStateChanged -> UpdateExpandOnHoverState's keep-expanded branch ->
// AnimateExpandOnHover(true) -> GroupData::Start -> Cancel() of the
// still-visible ended motion -> a spurious kCanceled whose handler
// dereferenced the already-nulled animation_perf_reporter_ (SIGSEGV at 0x10,
// crash report 2026-07-25). Patch 0057 makes the cascade safe: observer
// null-guards for the reporter plus a start-generation guard so the outer
// AnimationEnded cannot clear a motion installed re-entrantly during its own
// notification.

#include <memory>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/animation/browser_animation_controller.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/animations/tab_strip_animations.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/vertical/vertical_tab_strip_expand_on_hover_lock.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_container.h"
#include "ui/gfx/animation/animation_test_api.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace roamux {
namespace {

// Depth-first class-name lookup (same helper shape as the placement suite).
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

class RoamuxStripAnimationReentrancyTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxStripAnimationReentrancyTest() {
    features_.InitAndEnableFeature(features::kTabStripPosition);
  }

 protected:
  BrowserView* browser_view() {
    return BrowserView::GetBrowserViewForBrowser(browser());
  }

  // Vertical placement (left) so the vertical strip exists and displays.
  VerticalTabStripRegionView* SetUpVerticalStrip() {
    browser()->profile()->GetPrefs()->SetInteger(prefs::kTabStripPosition,
                                                 2 /* kLeft */);
    base::RunLoop().RunUntilIdle();
    browser_view()->DeprecatedLayoutImmediately();
    views::View* view =
        FindViewByClassName(browser_view(), "VerticalTabStripRegionView");
    return static_cast<VerticalTabStripRegionView*>(view);
  }

  BrowserAnimationController* animation_controller() {
    return BrowserAnimationController::From(browser());
  }

  base::test::ScopedFeatureList features_;
};

// T1 — the crash cascade, deterministic (rich animations force-disabled so
// GroupData::Start completes motions synchronously): a held kKeepExpanded lock
// at collapse completion re-enters the controller from inside the kEnded
// notification. Unpatched this SIGSEGVs on the nulled perf reporter; patched
// it completes with collapse state propagated and the keep-expanded
// re-expansion in effect.
IN_PROC_BROWSER_TEST_F(RoamuxStripAnimationReentrancyTest,
                       CollapseEndWithKeepExpandedLockDoesNotCrash) {
  auto rich_mode_resetter = gfx::AnimationTestApi::SetRichAnimationRenderMode(
      gfx::Animation::RichAnimationRenderMode::FORCE_DISABLED);
  ASSERT_FALSE(gfx::Animation::ShouldRenderRichAnimation());

  VerticalTabStripRegionView* strip = SetUpVerticalStrip();
  ASSERT_NE(strip, nullptr);

  std::unique_ptr<ExpandOnHoverLock> keep_expanded =
      strip->GetExpandOnHoverLock(ExpandOnHoverLockType::kKeepExpanded);
  ASSERT_NE(keep_expanded, nullptr);

  // The full cascade — kStarted, synchronous kEnded, re-entrant expand start,
  // spurious kCanceled — runs inside this call. Unpatched: SIGSEGV.
  animation_controller()->Start(TabStripAnimations::kVerticalTabStrip,
                                TabStripAnimations::kCollapse);

  // Patched: the cascade completed without crashing. The re-entrant
  // keep-expanded re-expansion ran through the same synchronous non-rich path,
  // so at rest nothing is animating and the strip is not left mid-motion.
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(animation_controller()->IsAnimating(
      TabStripAnimations::kVerticalTabStrip));
  // Final-state semantics: the collapse propagated to the state controller,
  // and the held keep-expanded lock re-expanded the strip on hover terms.
  EXPECT_TRUE(
      tabs::VerticalTabStripStateController::From(browser())->IsCollapsed());
  EXPECT_TRUE(strip->is_expanded_on_hover());
}

// T2 — nested-motion ownership under RICH animations: the replacement
// expand-on-hover motion started re-entrantly during the collapse's kEnded
// notification must survive the outer AnimationEnded unwind (the
// start-generation guard) and run to completion. A null-guard-only fix loses
// the replacement motion (the outer clear destroys it; the next tick would
// CHECK on current_motion_ and no expand kProgressed/kEnded ever arrives), so
// this test's wait design fails such a fix by construction.
IN_PROC_BROWSER_TEST_F(RoamuxStripAnimationReentrancyTest,
                       ReplacementMotionSurvivesEndedNotification) {
  auto rich_mode_resetter = gfx::AnimationTestApi::SetRichAnimationRenderMode(
      gfx::Animation::RichAnimationRenderMode::FORCE_ENABLED);
  ASSERT_TRUE(gfx::Animation::ShouldRenderRichAnimation());

  VerticalTabStripRegionView* strip = SetUpVerticalStrip();
  ASSERT_NE(strip, nullptr);

  // Browser tests do not reliably produce compositor frames, so drive the
  // animation clock manually through a test-owned container.
  auto container = base::MakeRefCounted<gfx::AnimationContainer>();
  animation_controller()->SetAnimationContainerForTesting(
      TabStripAnimations::kVerticalTabStrip, container.get());
  gfx::AnimationContainerTestApi time(container.get());

  std::unique_ptr<ExpandOnHoverLock> keep_expanded =
      strip->GetExpandOnHoverLock(ExpandOnHoverLockType::kKeepExpanded);
  ASSERT_NE(keep_expanded, nullptr);

  // Record the update stream. NOTE on attribution: this subscriber runs
  // AFTER the region view's (subscription order), so by the time it observes
  // the collapse's kEnded, the re-entrant cascade has already installed the
  // replacement motion and GetCurrentMotion reports kExpandOnHover — per-
  // motion attribution of kEnded is therefore ambiguous by design. The
  // unambiguous signals asserted instead: the replacement's own kStarted and
  // kProgressed (motion identity is stable at those dispatches), and TWO
  // kEnded events total (the collapse's and the replacement's). A null-guard-
  // only fix loses the replacement motion (the outer clear destroys it), so
  // no replacement kProgressed and only one kEnded ever arrive - this test
  // fails such a fix by construction.
  int ended_events = 0;
  bool replacement_started = false;
  bool replacement_progressed = false;
  base::CallbackListSubscription subscription =
      animation_controller()->Subscribe(
          TabStripAnimations::kVerticalTabStrip,
          base::BindRepeating(
              [](BrowserAnimationController* controller, int* ended_events,
                 bool* started, bool* progressed,
                 const BrowserAnimationController*,
                 BrowserAnimationUpdate update) {
                const BrowserAnimationMotion motion =
                    controller->GetCurrentMotion(
                        TabStripAnimations::kVerticalTabStrip);
                const bool is_replacement =
                    motion == TabStripAnimations::kExpandOnHover;
                switch (update) {
                  case BrowserAnimationUpdate::kEnded:
                    ++(*ended_events);
                    break;
                  case BrowserAnimationUpdate::kStarted:
                    if (is_replacement) {
                      *started = true;
                    }
                    break;
                  case BrowserAnimationUpdate::kProgressed:
                    if (is_replacement) {
                      *progressed = true;
                    }
                    break;
                  case BrowserAnimationUpdate::kCanceled:
                    break;
                }
              },
              animation_controller(), &ended_events, &replacement_started,
              &replacement_progressed));

  animation_controller()->Start(TabStripAnimations::kVerticalTabStrip,
                                TabStripAnimations::kCollapse);

  // Step the clock in small increments: the collapse runs to completion (its
  // AnimationEnded triggers the re-entrant replacement start), then further
  // steps drive the replacement to progress and end. Bounded by iteration
  // count, not wall clock.
  for (int i = 0; i < 200 && !(ended_events >= 2 && replacement_started &&
                               replacement_progressed);
       ++i) {
    time.IncrementTime(base::Milliseconds(50));
    base::RunLoop().RunUntilIdle();
  }
  EXPECT_GE(ended_events, 2);
  EXPECT_TRUE(replacement_started);
  EXPECT_TRUE(replacement_progressed);
  EXPECT_FALSE(animation_controller()->IsAnimating(
      TabStripAnimations::kVerticalTabStrip));
  EXPECT_TRUE(
      tabs::VerticalTabStripStateController::From(browser())->IsCollapsed());
  EXPECT_TRUE(strip->is_expanded_on_hover());
}

}  // namespace
}  // namespace roamux
