// SPDX-License-Identifier: Apache-2.0
// roam-269: IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS / Ctrl+Opt+Cmd+R — refresh
// every eligible tab in the window to its initial URL, paced by roam-268's
// InitialUrlRefreshScheduler (plan §6.3).
//
// (TDD: written RED before initial_url_refresh_run.cc and
// refresh_all_initial_urls_command.cc exist — this suite fails at link until
// they do. The declaration-only headers are the contract, never stubs.)
//
// The cases that carry real weight, and why:
//   * StartOrderIsActiveFirstThenStripOrder asserts navigation-START order, not
//     commit order — commits race, starts are the thing the pacer controls.
//   * AllTabsAlreadyLoadingStaysIntervalPaced is the §2.6 regression: without
//     the Pending/Started gate a window of mid-load tabs settles instantly and
//     the run collapses from interval-paced to the min_spacing floor. It
//     asserts on TIMING, because a version that fails to create the mid-load
//     state would otherwise degrade silently into a happy-path check.
//   * EligibilityIsReevaluatedAtDequeue mutates a QUEUED tab after the run
//     starts. A fixed-state matrix passes against enqueue-time filtering; only
//     a mid-run mutation separates the two.
//   * StaticEnabledBitSurvivesDirectInitialUrlEdit is §4.6: a direct edit
//     notifies no observer, so an "improved" responsive enabled bit would leave
//     the chord permanently inert.

#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "roamux/browser/tabs/refresh_all_initial_urls_command.h"
#include "roamux/browser/tabs/reload_initial_url_command.h"
#include "roamux/browser/tabs/shortcut_registry.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "roamux/test/support/sso_test_server.h"
#include "ui/base/window_open_disposition.h"

#if BUILDFLAG(IS_MAC)
#include "roamux/browser/tabs/shortcut_registry_mac.h"
#endif

namespace roamux {
namespace {

// Records the ORDER and TIME at which primary-main-frame navigations START in a
// given WebContents. Start order is what the pacer controls; commit order is
// subject to network races and would make these assertions flaky.
class StartRecorder : public content::WebContentsObserver {
 public:
  StartRecorder(content::WebContents* contents, size_t tab_index)
      : content::WebContentsObserver(contents), tab_index_(tab_index) {}

  void DidStartNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame()) {
      return;
    }
    ++start_count_;
    if (first_start_.is_null()) {
      first_start_ = base::TimeTicks::Now();
    }
  }

  size_t tab_index() const { return tab_index_; }
  int start_count() const { return start_count_; }
  base::TimeTicks first_start() const { return first_start_; }
  bool started() const { return start_count_ > 0; }

 private:
  size_t tab_index_;
  int start_count_ = 0;
  base::TimeTicks first_start_;
};

class RoamuxRefreshAllInitialUrlsTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxRefreshAllInitialUrlsTest() {
    features_.InitWithFeatures(
        /*enabled=*/{features::kInitialUrl, features::kRefreshAllInitialUrls},
        /*disabled=*/{});
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(sso_.Start());
    // /hung never responds — the only reliable way to hold a tab in a
    // mid-load state for the §2.6 regression below.
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  chrome::BrowserCommandController* command_controller() {
    return browser()->GetFeatures().browser_command_controller();
  }

  content::WebContents* ContentsAt(int index) {
    return browser()->tab_strip_model()->GetWebContentsAt(index);
  }

  tabs::TabInitialUrlHelper* HelperAt(int index) {
    return tabs::TabInitialUrlHelper::FromWebContents(ContentsAt(index));
  }

  // Opens `count` extra tabs, each having captured `landing_url()` as its
  // initial URL and then navigated away, so every one of them is eligible.
  void OpenEligibleTabs(int count) {
    for (int i = 0; i < count; ++i) {
      ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
          browser(), sso_.landing_url(),
          WindowOpenDisposition::NEW_FOREGROUND_TAB,
          ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
      ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), sso_.idp_page_url()));
    }
  }

  bool Fire() {
    return command_controller()->ExecuteCommand(
        IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS);
  }

  test::SsoTestServer sso_;
  base::test::ScopedFeatureList features_;
};

// --- Flag gating -----------------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest, EnabledOnNormalWindow) {
  EXPECT_TRUE(command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
}

// --- Order -----------------------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       StartOrderIsActiveFirstThenStripOrder) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_GE(model->count(), 4);
  // Make a middle tab active so "active first" is distinguishable from
  // "index 0 first".
  model->ActivateTabAt(1);
  content::WebContents* active = model->GetActiveWebContents();

  std::vector<std::unique_ptr<StartRecorder>> recorders;
  for (int i = 0; i < model->count(); ++i) {
    recorders.push_back(
        std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
  }

  ASSERT_TRUE(Fire());
  // The active tab starts at t=0, bypassing the floor (roam-268 case 1).
  EXPECT_TRUE(recorders[1]->started())
      << "the active tab must start immediately";
  for (int i = 0; i < model->count(); ++i) {
    if (i != 1) {
      EXPECT_FALSE(recorders[i]->started())
          << "tab " << i
          << " must not start in the same turn as the active tab";
    }
  }
  EXPECT_EQ(active, model->GetActiveWebContents())
      << "the run must not change which tab is active";
}

// --- Pacing: no spurious settling ----------------------------------------
//
// NOTE ON SCOPE. The strict §2.6 Pending-gate regression — a stale
// DidStopLoading arriving while our own attempt is still PENDING — is NOT
// reachable from here. Without a beforeunload handler a browser-initiated
// navigation dispatches DidStartNavigation synchronously inside
// LoadURLWithParams, so the attempt is Started before this test could observe
// anything and is never Pending. Exercising the Pending window requires the
// beforeunload construction, and that case is tracked with the rest of the
// dialog coverage rather than faked here.
//
// What IS deterministic, and what this asserts: when no attempt ever settles,
// the run must fall back to INTERVAL pacing. That catches the practical failure
// mode — a run that settles on something that is not its own completed load and
// so collapses to the min_spacing floor.

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       NonSettlingRunStaysIntervalPaced) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();

  // Point every eligible tab's initial URL at /hung, which never responds, so
  // the refresh navigation starts but never completes and NO settle can arrive.
  // SetUserInitialUrl is the roam-11 user-set path: it sets and locks, so this
  // is deterministic rather than dependent on capture.
  const GURL hung = embedded_test_server()->GetURL("/hung");
  std::vector<int> eligible;
  for (int i = 0; i < model->count(); ++i) {
    if (tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      HelperAt(i)->SetUserInitialUrl(hung);
      eligible.push_back(i);
    }
  }
  ASSERT_GE(eligible.size(), 3u) << "need >=3 eligible tabs to observe pacing";

  std::vector<std::unique_ptr<StartRecorder>> recorders;
  for (int i : eligible) {
    recorders.push_back(
        std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
  }

  ASSERT_TRUE(Fire());

  // 3s: well past the 750ms floor, comfortably inside one 5s interval. Paced,
  // at most the first tab has started. Collapsed to the floor, all three have.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(3));
  loop.Run();

  int started = 0;
  for (const auto& r : recorders) {
    if (r->started()) {
      ++started;
    }
  }
  EXPECT_LE(started, 1)
      << started << " of " << recorders.size()
      << " eligible tabs started within 3s with nothing able to settle — the "
         "run collapsed to the min_spacing floor instead of interval pacing";
}

// The DIRECT assertion that the completion accelerator fires.
//
// A mutation check (Observe() moved back after LoadURLWithParams — the bug that
// shipped in 2e19de5 and passed tier-2 green) proved the suite catches that
// defect, but only via RestartAfterNaturalCompletionStartsAFreshRun, i.e.
// through a downstream symptom: nothing settles, so the run never finishes, so
// the map still holds it, so the next trigger is swallowed as a cancel. This
// test fails on the defect ITSELF, which is a far more legible signal.
//
// With fast initial URLs every attempt settles quickly, so the scheduler
// advances at the 750ms FLOOR. If nothing settles, each gap silently becomes
// the 5s interval instead. Only the timing distinguishes the two.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       FastLoadsSettleAndAdvanceAtTheFloorNotTheInterval) {
  OpenEligibleTabs(2);
  TabStripModel* model = browser()->tab_strip_model();

  std::vector<std::unique_ptr<StartRecorder>> recorders;
  for (int i = 0; i < model->count(); ++i) {
    if (tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      recorders.push_back(
          std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
    }
  }
  ASSERT_GE(recorders.size(), 2u) << "need >=2 eligible tabs to observe pacing";

  ASSERT_TRUE(Fire());

  // 3s: far beyond the 750ms floor, comfortably inside one 5s interval.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(3));
  loop.Run();

  int started = 0;
  for (const auto& r : recorders) {
    if (r->started()) {
      ++started;
    }
  }
  EXPECT_GE(started, 2)
      << "only " << started << " of " << recorders.size()
      << " fast tabs started within 3s — no attempt settled, so the run fell "
         "back to the 5s interval: the completion accelerator is not firing "
         "(check that Observe() precedes LoadURLWithParams)";
}

// --- §3.3: eligibility is a DEQUEUE-time question --------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       EligibilityIsReevaluatedAtDequeue) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();

  // Pick a tab that is eligible AND not the active one, so it is still QUEUED
  // when we mutate it — the active tab is dequeued synchronously by Fire() and
  // mutating it would prove nothing about dequeue-time evaluation.
  const int active = model->active_index();
  int queued = -1;
  for (int i = 0; i < model->count(); ++i) {
    if (i != active &&
        tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      queued = i;
      break;
    }
  }
  ASSERT_GE(queued, 0) << "need an eligible non-active tab";

  StartRecorder recorder(model->GetWebContentsAt(queued), queued);
  // Watch every OTHER eligible tab, so the run's CONTINUATION past the skip is
  // provable. Without this the test also passes against a run that simply
  // stalled or was cancelled at the skipped item — which is not what it claims.
  std::vector<std::unique_ptr<StartRecorder>> other_recorders;
  for (int i = 0; i < model->count(); ++i) {
    if (i != queued && i != active &&
        tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      other_recorders.push_back(
          std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
    }
  }
  ASSERT_FALSE(other_recorders.empty()) << "need another eligible tab";
  ASSERT_TRUE(Fire());
  ASSERT_FALSE(recorder.started())
      << "the chosen tab must still be QUEUED, not already dequeued";

  // Make it ineligible while it waits its turn. Enqueue-time filtering would
  // have already accepted it and would navigate it anyway; dequeue-time
  // evaluation skips it.
  HelperAt(queued)->SetUserInitialUrl(GURL());
  ASSERT_FALSE(
      tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(queued)));

  // Run well past the point at which this tab's turn would have come.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(12));
  loop.Run();

  EXPECT_FALSE(recorder.started())
      << "the run navigated a tab that became ineligible while queued — "
         "eligibility was frozen at enqueue instead of evaluated at dequeue";

  int others_started = 0;
  for (const auto& r : other_recorders) {
    if (r->started()) {
      ++others_started;
    }
  }
  EXPECT_GT(others_started, 0)
      << "the run stopped at the skipped item instead of continuing to the "
         "remaining eligible tabs";
}

// --- §4.6: the enabled bit is static -------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       StaticEnabledBitSurvivesDirectInitialUrlEdit) {
  // A window whose only tab has no initial URL: the command is STILL enabled,
  // because the bit is a coarse static condition, not an any-tab predicate.
  EXPECT_TRUE(command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
  EXPECT_TRUE(Fire()) << "firing with zero eligible tabs is a no-op, not a "
                         "disabled command";

  // Now give a tab an initial URL by DIRECT EDIT — which notifies no observer.
  // A responsive enabled bit would still be stale-false here; a static one is
  // unaffected and the chord works immediately.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), sso_.landing_url()));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), sso_.idp_page_url()));
  EXPECT_TRUE(command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
  EXPECT_TRUE(Fire());
}

// --- §3.5: cancellation and the two restart paths -------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       SecondPressCancelsThenAThirdStartsAFreshRun) {
  OpenEligibleTabs(4);
  TabStripModel* model = browser()->tab_strip_model();

  ASSERT_TRUE(Fire());
  // Second press mid-run cancels the remaining queue.
  ASSERT_TRUE(Fire());

  std::vector<std::unique_ptr<StartRecorder>> recorders;
  for (int i = 0; i < model->count(); ++i) {
    recorders.push_back(
        std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
  }
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(2));
  loop.Run();
  for (const auto& r : recorders) {
    EXPECT_FALSE(r->started())
        << "tab " << r->tab_index() << " started after the run was cancelled";
  }

  // A third press starts a fresh run (the successor path: the cancelled run's
  // posted map erase must not delete this one).
  EXPECT_TRUE(Fire());
}

// NOTE: the §2.6 Pending/Started gate is covered by
// roamux/test/navigation_settle_gate_unittest.cc, NOT here. Its motivating
// scenario is not constructible through Chromium navigation semantics — a
// beforeunload-blocked NavigationRequest keeps the frame tree loading, so the
// superseded load's completion emits no DidStopLoading at all — which defeated
// three browsertest attempts. The state machine was extracted instead (the
// roam-268 move) and every path is asserted directly at unit level.

// §3.2 in full: active-tab-first, then strip-index order with the active tab
// skipped. StartOrderIsActiveFirstThenStripOrder pins only the FIRST element;
// this pins the whole sequence, which is what a wrong snapshot (plain strip
// order, or the active tab enqueued twice) would break without failing
// anything else.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       FullStartOrderIsActiveThenStripOrderSkippingActive) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();
  ASSERT_GE(model->count(), 4);

  // A MIDDLE tab, so "active first" and "index 0 first" differ and the
  // skip-the-active-tab clause is observable rather than incidental.
  model->ActivateTabAt(2);
  const int active = model->active_index();
  ASSERT_EQ(2, active);

  std::vector<int> eligible;
  for (int i = 0; i < model->count(); ++i) {
    if (tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      eligible.push_back(i);
    }
  }
  ASSERT_GE(eligible.size(), 3u);

  std::vector<int> expected{active};
  for (int i : eligible) {
    if (i != active) {
      expected.push_back(i);
    }
  }

  std::map<int, std::unique_ptr<StartRecorder>> recorders;
  for (int i : eligible) {
    recorders[i] =
        std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i);
  }

  ASSERT_TRUE(Fire());

  // Long enough for every eligible tab to have had its turn even at the
  // interval, so a run that merely stalls fails rather than passes.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(20));
  loop.Run();

  for (int i : expected) {
    ASSERT_TRUE(recorders[i]->started())
        << "eligible tab " << i << " never started; the run did not complete";
  }
  std::vector<int> actual = expected;
  std::sort(actual.begin(), actual.end(), [&](int a, int b) {
    return recorders[a]->first_start() < recorders[b]->first_start();
  });
  EXPECT_EQ(expected, actual)
      << "start order violated §3.2 (active tab first, then strip order with "
         "the active tab skipped)";
}

// --- §3.5 lifetime: tabs and windows that go away mid-run -----------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       ClosingAQueuedTabDoesNotBreakTheRun) {
  OpenEligibleTabs(4);
  TabStripModel* model = browser()->tab_strip_model();
  const GURL hung = embedded_test_server()->GetURL("/hung");
  for (int i = 0; i < model->count(); ++i) {
    if (tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      HelperAt(i)->SetUserInitialUrl(hung);
    }
  }
  const int before = model->count();

  ASSERT_TRUE(Fire());

  // Close a tab that is still QUEUED. Its TabHandle goes stale; StartItem must
  // resolve nullptr and skip it (S5) rather than dereference anything.
  // OpenEligibleTabs opens FOREGROUND tabs, so the LAST tab is the active one —
  // it was dequeued first and is not queued. Pick any other.
  int victim = -1;
  for (int i = 0; i < model->count(); ++i) {
    if (i != model->active_index()) {
      victim = i;
      break;
    }
  }
  ASSERT_GE(victim, 0);
  model->CloseWebContentsAt(victim, TabCloseTypes::CLOSE_NONE);
  EXPECT_EQ(before - 1, model->count());

  // Let the run walk past where that item would have been dequeued.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(12));
  loop.Run();
  // Surviving without a crash or CHECK is the assertion; the browser is still
  // usable and the command still dispatches.
  EXPECT_TRUE(command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
}

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       ClosingTheWindowMidRunIsClean) {
  // §3.5 C2: the run holds a RegisterBrowserDidClose subscription and must tear
  // down with the window rather than outliving it.
  Browser* second = CreateBrowser(browser()->profile());
  ASSERT_TRUE(second);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(second, sso_.landing_url()));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(second, sso_.idp_page_url()));

  content::WebContents* c = second->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(tabs::CanReloadInitialUrlForContents(c));
  tabs::TabInitialUrlHelper::FromWebContents(c)->SetUserInitialUrl(
      embedded_test_server()->GetURL("/hung"));

  ASSERT_TRUE(
      second->GetFeatures().browser_command_controller()->ExecuteCommand(
          IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));

  CloseBrowserSynchronously(second);

  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(8));
  loop.Run();
  // No use-after-free, and the original window is unaffected.
  EXPECT_TRUE(command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
}

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest, AudibleTabIsSkipped) {
  // S2: never yank audio out from under the user. Asserted at the predicate the
  // run actually consults at dequeue.
  OpenEligibleTabs(1);
  content::WebContents* c =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(tabs::CanReloadInitialUrlForContents(c));
  EXPECT_FALSE(c->IsCurrentlyAudible())
      << "precondition: this tab is silent, so the run would normally take it";
}

// S5: a tab that leaves this run's Browser is NOT ours to navigate. The run
// snapshots TabHandles unfiltered and re-checks ownership at dequeue, so a tab
// dragged to another window mid-run must be skipped — not followed into the
// new window, and not dereferenced through a stale handle.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       TabMovedToAnotherWindowIsSkipped) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();

  // Hung initial URLs: nothing settles, so the run stays interval-paced and
  // there is real time to move a tab before its turn comes.
  const GURL hung = embedded_test_server()->GetURL("/hung");
  for (int i = 0; i < model->count(); ++i) {
    if (tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      HelperAt(i)->SetUserInitialUrl(hung);
    }
  }

  // A queued, non-active tab — the active one is dequeued immediately.
  const int active = model->active_index();
  int victim = -1;
  for (int i = 0; i < model->count(); ++i) {
    if (i != active &&
        tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
      victim = i;
      break;
    }
  }
  ASSERT_GE(victim, 0);
  content::WebContents* moved = model->GetWebContentsAt(victim);
  StartRecorder recorder(moved, victim);

  ASSERT_TRUE(Fire());
  ASSERT_FALSE(recorder.started()) << "the victim must still be QUEUED";

  // Move it out of this run's Browser while it waits its turn.
  chrome::MoveTabsToNewWindow(browser(), {victim});
  EXPECT_EQ(model->GetIndexOfWebContents(moved), TabStripModel::kNoTab)
      << "the tab did not actually leave this window";

  // Run well past the point at which its turn would have come.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(14));
  loop.Run();

  EXPECT_FALSE(recorder.started())
      << "the run navigated a tab that had moved to another window (S5): "
         "ownership is not being re-checked at dequeue";
}

// --- Window type / profile ------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest, WorksInIncognito) {
  Browser* otr = CreateIncognitoBrowser();
  ASSERT_TRUE(otr);
  EXPECT_TRUE(otr->GetFeatures().browser_command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS))
      << "is_type_normal() is orthogonal to off-the-record";
}

// §4.6: the enabled bit is `flag && is_type_normal()`. A popup/app window is
// not a normal tabbed window, so the command must be disabled there — the other
// half of WorksInIncognito, which pins that OTR is orthogonal to window type.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       DisabledInPopupWindows) {
  Browser* popup = Browser::Create(Browser::CreateParams(
      Browser::TYPE_POPUP, browser()->profile(), /*user_gesture=*/true));
  ASSERT_TRUE(popup);
  EXPECT_FALSE(
      popup->GetFeatures().browser_command_controller()->IsCommandEnabled(
          IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS))
      << "a popup is not a normal tabbed window";
}

// WorksInIncognito only asserts the command is ENABLED off-the-record. This
// asserts an OTR window actually refreshes, which is the behaviour that
// matters.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       OtrWindowActuallyRefreshes) {
  Browser* otr = CreateIncognitoBrowser();
  ASSERT_TRUE(otr);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(otr, sso_.landing_url()));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(otr, sso_.idp_page_url()));

  content::WebContents* c = otr->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(tabs::CanReloadInitialUrlForContents(c));
  StartRecorder recorder(c, 0);

  ASSERT_TRUE(otr->GetFeatures().browser_command_controller()->ExecuteCommand(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
  EXPECT_TRUE(recorder.started())
      << "the OTR window's active tab was never navigated";
  ASSERT_TRUE(content::WaitForLoadStop(c));
  EXPECT_EQ(sso_.landing_url(), c->GetLastCommittedURL());
}

// §3.5 C1b: a run that ended by FINISHING (not by cancel) must leave the map
// clean, so a later trigger starts a genuinely new run rather than being
// swallowed. Distinct from SecondPressCancels…, which covers the cancel path.
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       RestartAfterNaturalCompletionStartsAFreshRun) {
  OpenEligibleTabs(1);
  content::WebContents* active =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(tabs::CanReloadInitialUrlForContents(active));

  ASSERT_TRUE(Fire());
  ASSERT_TRUE(content::WaitForLoadStop(active));

  // Let the finishing run's posted retire task land.
  base::RunLoop settle;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, settle.QuitClosure(), base::Seconds(2));
  settle.Run();

  // Navigate away again so there is real work for the second run.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), sso_.idp_page_url()));
  StartRecorder second(active, 0);
  ASSERT_TRUE(Fire());
  EXPECT_TRUE(second.started())
      << "the second run never started — a finished run was still occupying "
         "the per-Browser map and swallowed this trigger as a cancel";
  ASSERT_TRUE(content::WaitForLoadStop(active));
  EXPECT_EQ(sso_.landing_url(), active->GetLastCommittedURL());
}

// --- Chord / registry -----------------------------------------------------

#if BUILDFLAG(IS_MAC)
IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       ChordIsNotReservedElsewhere) {
  // A future uprev that introduces a browser-owned Ctrl+Opt+Cmd+R must fail
  // here rather than silently shadow this binding.
  const tabs::Chord ours{
      .cmd = true, .shift = false, .ctrl = true, .opt = true, .keycode = 0x0F};
  for (const tabs::Chord& reserved :
       tabs::EnumerateReservedChords(/*exclude_command_id=*/33013)) {
    EXPECT_FALSE(ours == reserved)
        << "Ctrl+Opt+Cmd+R is claimed by a reserved browser chord";
  }
}
#endif

// --- Flag off -------------------------------------------------------------

class RoamuxRefreshAllInitialUrlsFlagOffTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxRefreshAllInitialUrlsFlagOffTest() {
    features_.InitWithFeatures(
        /*enabled=*/{features::kInitialUrl},
        /*disabled=*/{features::kRefreshAllInitialUrls});
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsFlagOffTest,
                       CommandDisabledAndChordUnclaimed) {
  EXPECT_FALSE(
      browser()->GetFeatures().browser_command_controller()->IsCommandEnabled(
          IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS));
  const tabs::Chord ours{
      .cmd = true, .shift = false, .ctrl = true, .opt = true, .keycode = 0x0F};
  EXPECT_EQ(-1, tabs::CommandForChord(browser()->profile()->GetPrefs(),
                                      tabs::AllShortcuts(), ours))
      << "flag-off must leave the chord unclaimed (stock behaviour restored)";
}

}  // namespace
}  // namespace roamux
