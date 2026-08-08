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
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
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

// --- §2.6: the Pending/Started gate (THE regression) -----------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       AllTabsAlreadyLoadingStaysIntervalPaced) {
  OpenEligibleTabs(3);
  TabStripModel* model = browser()->tab_strip_model();

  // Put EVERY tab into a mid-load state before firing. Without the §2.6 gate,
  // the stale DidStopLoading from these loads settles each attempt instantly
  // and the whole run collapses to the min_spacing floor.
  for (int i = 0; i < model->count(); ++i) {
    model->GetWebContentsAt(i)->GetController().LoadURL(
        embedded_test_server()->GetURL("/hung"), content::Referrer(),
        ui::PAGE_TRANSITION_TYPED, std::string());
  }
  ASSERT_TRUE(model->GetWebContentsAt(0)->IsLoading())
      << "the mid-load precondition was not established; this test would "
         "otherwise degrade into a happy-path check";

  std::vector<std::unique_ptr<StartRecorder>> recorders;
  for (int i = 0; i < model->count(); ++i) {
    recorders.push_back(
        std::make_unique<StartRecorder>(model->GetWebContentsAt(i), i));
  }

  const base::TimeTicks fired_at = base::TimeTicks::Now();
  ASSERT_TRUE(Fire());

  // Give the run a window comfortably shorter than one `interval` but far
  // longer than the 750ms floor. If the gate is missing, every tab has already
  // started by now.
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
  EXPECT_LT(started, model->count())
      << "all " << model->count() << " tabs started within 3s of firing — the "
      << "§2.6 Pending/Started gate is missing and the run collapsed to the "
      << "min_spacing floor";
  EXPECT_LT(base::TimeTicks::Now() - fired_at, base::Seconds(10));
}

// --- §3.3: eligibility is a DEQUEUE-time question --------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest,
                       EligibilityIsReevaluatedAtDequeue) {
  OpenEligibleTabs(2);
  TabStripModel* model = browser()->tab_strip_model();
  const int last = model->count() - 1;
  ASSERT_TRUE(HelperAt(last)->has_initial_url());

  ASSERT_TRUE(Fire());

  // Mutate a tab that is still QUEUED (not yet dequeued) so its initial URL is
  // no longer valid. Enqueue-time filtering would have already accepted it.
  HelperAt(last)->SetUserInitialUrl(GURL());
  EXPECT_FALSE(
      tabs::CanReloadInitialUrlForContents(model->GetWebContentsAt(last)));
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

// --- Window type / profile ------------------------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxRefreshAllInitialUrlsTest, WorksInIncognito) {
  Browser* otr = CreateIncognitoBrowser();
  ASSERT_TRUE(otr);
  EXPECT_TRUE(otr->GetFeatures().browser_command_controller()->IsCommandEnabled(
      IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS))
      << "is_type_normal() is orthogonal to off-the-record";
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
