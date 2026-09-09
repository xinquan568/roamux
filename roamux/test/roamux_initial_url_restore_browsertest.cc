// SPDX-License-Identifier: Apache-2.0
// roam-14 (I-2.5, §4.7): full-restart SessionRestore of an edited (locked)
// initial URL — the PRODUCER path (capture/edit writes via
// SessionService::AddTabExtraData), restored across an in-harness restart.

#include "base/test/scoped_feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_features.h"
#include "net/test/embedded_test_server/controllable_http_response.h"
#include "net/http/http_status_code.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/tabs/tab_uid_tab_helper.h"
#include "roamux/browser/ui/tabs/edit_initial_url_dialog.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"

namespace roamux {
namespace {

// SessionRestore requires a persistent profile; PRE_ seeds it, the twin reads.
class RoamuxInitialUrlRestoreTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxInitialUrlRestoreTest() {
    features_.InitAndEnableFeature(features::kInitialUrl);
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    // Restore the previous session on the next launch (the PRE_ pair).
    SessionStartupPref::SetStartupPref(
        browser()->profile(), SessionStartupPref(SessionStartupPref::LAST));
    InProcessBrowserTest::SetUpOnMainThread();
  }

 protected:
  base::test::ScopedFeatureList features_;
};

// Two-hop (Step-8 finding 1): the value must survive a SECOND restart, which
// only holds if the restore path re-persisted under the new tab id.
IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       PRE_PRE_TwoHopRestartKeepsEditedValue) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(
      tabs::SubmitEditInitialUrlForTesting(contents, "https://edited.test/"));
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       PRE_TwoHopRestartKeepsEditedValue) {
  // First restart: the restore path re-persists on the first navigation.
  TabStripModel* tab_strip = browser()->tab_strip_model();
  for (int i = 0; i < tab_strip->count(); ++i) {
    ASSERT_TRUE(content::WaitForLoadStop(tab_strip->GetWebContentsAt(i)));
  }
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       TwoHopRestartKeepsEditedValue) {
  // Second restart: the value is still present, proving the re-persist held.
  TabStripModel* tab_strip = browser()->tab_strip_model();
  tabs::TabInitialUrlHelper* helper = nullptr;
  for (int i = 0; i < tab_strip->count(); ++i) {
    ASSERT_TRUE(content::WaitForLoadStop(tab_strip->GetWebContentsAt(i)));
    tabs::TabInitialUrlHelper* candidate =
        tabs::TabInitialUrlHelper::FromWebContents(
            tab_strip->GetWebContentsAt(i));
    if (candidate && candidate->has_initial_url()) {
      helper = candidate;
      break;
    }
  }
  ASSERT_NE(nullptr, helper) << "the edited value did not survive two restarts";
  EXPECT_EQ(GURL("https://edited.test/"), helper->initial_url());
  EXPECT_TRUE(helper->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       PRE_RestartRestoresEditedValue) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  // Edit (locks) — exercises the SessionService producer write.
  ASSERT_TRUE(
      tabs::SubmitEditInitialUrlForTesting(contents, "https://edited.test/"));
  ASSERT_TRUE(
      tabs::TabInitialUrlHelper::FromWebContents(contents)->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       RestartRestoresEditedValue) {
  // The browser was restarted with the previous session restored; the restored
  // tab joins the fresh startup tab, so scan for the one carrying our helper.
  TabStripModel* tab_strip = browser()->tab_strip_model();
  tabs::TabInitialUrlHelper* helper = nullptr;
  for (int i = 0; i < tab_strip->count(); ++i) {
    ASSERT_TRUE(content::WaitForLoadStop(tab_strip->GetWebContentsAt(i)));
    tabs::TabInitialUrlHelper* candidate =
        tabs::TabInitialUrlHelper::FromWebContents(
            tab_strip->GetWebContentsAt(i));
    if (candidate && candidate->has_initial_url()) {
      helper = candidate;
      break;
    }
  }
  // The producer write (edit) persisted through the session service and the
  // restore consumer pre-armed it before the restore navigation (§4.7).
  ASSERT_NE(nullptr, helper) << "no restored tab carried the initial URL";
  EXPECT_EQ(GURL("https://edited.test/"), helper->initial_url());
  EXPECT_TRUE(helper->is_user_locked());
}


// ---------------------------------------------------------------------------
// roam-320: a session-log REBUILD must not erase the initial URL.
//
// SessionService periodically re-derives the whole session file from live
// browser state (every 250 commands, when a navigation index leaves the
// archived range, when history entries are deleted, at service construction).
// Its builders emit no extra-data command, so the append-only "roamux.initial_url"
// write is dropped; on relaunch nothing is restored, the helper stays
// uncaptured, and the restored tab's first commit becomes its initial URL.
// These cases force the rebuild explicitly, because the pre-existing restart
// coverage never crosses one.
// ---------------------------------------------------------------------------

void ForceSessionRebuild(Profile* profile) {
  SessionService* session_service = SessionServiceFactory::GetForProfile(profile);
  ASSERT_TRUE(session_service);
  // Public API; upstream's own session_restore_browsertest.cc drives it the
  // same way. Clears the pending commands and rebuilds from live state.
  session_service->ResetFromCurrentBrowsers();
}

// The subject must be located by an identifier INDEPENDENT of the value under
// test — a tab that still holds a stale value would otherwise be mistaken for a
// survivor — and the expected value must cross processes durably, because the
// embedded test server picks a fresh port per process. Both ride the profile
// directory, which survives from a PRE_ test to its twin.
base::FilePath CrossProcessPath(Browser* browser, const char* name) {
  return browser->profile()->GetPath().AppendASCII(name);
}

void WriteCrossProcess(Browser* browser, const char* name,
                       const std::string& value) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  ASSERT_TRUE(base::WriteFile(CrossProcessPath(browser, name), value));
}

std::string ReadCrossProcess(Browser* browser, const char* name) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::string value;
  EXPECT_TRUE(base::ReadFileToString(CrossProcessPath(browser, name), &value));
  return value;
}

// Records the subject's durable tab uid and the value the rebuild must preserve.
void RecordSubject(Browser* browser, content::WebContents* subject) {
  tabs::TabUidTabHelper* uid_helper =
      tabs::TabUidTabHelper::FromWebContents(subject);
  ASSERT_TRUE(uid_helper);
  ASSERT_FALSE(uid_helper->uid().empty());
  WriteCrossProcess(browser, "roam320-subject-uid", uid_helper->uid());
  tabs::TabInitialUrlHelper* helper =
      tabs::TabInitialUrlHelper::FromWebContents(subject);
  ASSERT_TRUE(helper && helper->has_initial_url());
  WriteCrossProcess(browser, "roam320-expected-url",
                    helper->initial_url().spec());
}

// Finds the recorded subject by uid, never by the value under test.
tabs::TabInitialUrlHelper* FindRecordedSubject(Browser* browser) {
  const std::string uid = ReadCrossProcess(browser, "roam320-subject-uid");
  EXPECT_FALSE(uid.empty());
  TabStripModel* tab_strip = browser->tab_strip_model();
  for (int i = 0; i < tab_strip->count(); ++i) {
    content::WebContents* contents = tab_strip->GetWebContentsAt(i);
    EXPECT_TRUE(content::WaitForLoadStop(contents));
    tabs::TabUidTabHelper* uid_helper =
        tabs::TabUidTabHelper::FromWebContents(contents);
    if (uid_helper && uid_helper->uid() == uid) {
      return tabs::TabInitialUrlHelper::FromWebContents(contents);
    }
  }
  return nullptr;
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       PRE_RebuildKeepsCapturedInitialUrl) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  tabs::TabInitialUrlHelper* helper =
      tabs::TabInitialUrlHelper::FromWebContents(contents);
  ASSERT_TRUE(helper && helper->has_initial_url());
  const GURL captured = helper->initial_url();

  // Move on, so the tab's CURRENT page differs from its initial URL: that is
  // what a relaunch wrongly captures once the extra data is gone.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title2.html")));
  ASSERT_EQ(captured, helper->initial_url());
  RecordSubject(browser(), contents);

  ForceSessionRebuild(browser()->profile());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       RebuildKeepsCapturedInitialUrl) {
  // The expected URL is the one recorded before the restart: this process's
  // test server listens on a different port.
  const std::string expected = ReadCrossProcess(browser(), "roam320-expected-url");
  tabs::TabInitialUrlHelper* helper = FindRecordedSubject(browser());
  ASSERT_NE(nullptr, helper) << "the rebuild erased the initial URL";
  EXPECT_EQ(GURL(expected), helper->initial_url());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       PRE_RebuildKeepsEditedLockedValue) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(
      tabs::SubmitEditInitialUrlForTesting(contents, "https://edited.test/"));
  ASSERT_TRUE(
      tabs::TabInitialUrlHelper::FromWebContents(contents)->is_user_locked());
  RecordSubject(browser(), contents);

  ForceSessionRebuild(browser()->profile());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRestoreTest,
                       RebuildKeepsEditedLockedValue) {
  tabs::TabInitialUrlHelper* helper = FindRecordedSubject(browser());
  ASSERT_NE(nullptr, helper) << "the rebuild erased the edited value";
  EXPECT_EQ(GURL("https://edited.test/"), helper->initial_url());
  // The lock bit rides the same erased command.
  EXPECT_TRUE(helper->is_user_locked());
}

// ---------------------------------------------------------------------------
// The replacement-discard case: a discard gives the tab NEW contents with a NEW
// SessionID, and the helper's state is copied across without being persisted.
// Anything keyed by the old id is therefore empty under the id the rebuild
// serializes; only a live read of the replacement's helper is correct.
// ---------------------------------------------------------------------------
class RoamuxInitialUrlRebuildDiscardTest : public RoamuxInitialUrlRestoreTest {
 public:
  RoamuxInitialUrlRebuildDiscardTest() {
    // Discard only REPLACES the contents on this branch; with the feature on it
    // can keep them, and the changed-SessionID condition would never arise.
    discard_features_.InitAndDisableFeature(::features::kWebContentsDiscard);
  }

 protected:
  base::test::ScopedFeatureList discard_features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRebuildDiscardTest,
                       PRE_RebuildAfterReplacementDiscardKeepsValue) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  ASSERT_TRUE(AddTabAtIndex(1, embedded_test_server()->GetURL("/title1.html"),
                            ui::PAGE_TRANSITION_TYPED));
  TabStripModel* tab_strip = browser()->tab_strip_model();
  content::WebContents* before = tab_strip->GetWebContentsAt(1);
  ASSERT_TRUE(
      tabs::SubmitEditInitialUrlForTesting(before, "https://edited.test/"));
  const SessionID id_before = sessions::SessionTabHelper::IdForTab(before);
  ASSERT_TRUE(id_before.is_valid());

  TabListInterface* tab_list = TabListInterface::From(browser());
  ASSERT_NE(nullptr, tab_list);
  ASSERT_NE(nullptr, tab_list->DiscardTab(tab_list->GetTab(1)->GetHandle()));
  base::RunLoop().RunUntilIdle();

  content::WebContents* after = tab_strip->GetWebContentsAt(1);
  // The preconditions this case exists to exercise; without them the test would
  // silently degrade into the plain rebuild case.
  ASSERT_NE(before, after) << "discard did not replace the contents";
  const SessionID id_after = sessions::SessionTabHelper::IdForTab(after);
  ASSERT_NE(id_before.id(), id_after.id()) << "the SessionID did not change";
  tabs::TabInitialUrlHelper* helper =
      tabs::TabInitialUrlHelper::FromWebContents(after);
  ASSERT_TRUE(helper && helper->is_user_locked());
  // Record AFTER the discard: the subject is the replacement contents, and the
  // sibling tab at index 0 also carries a (different, unlocked) captured value,
  // so a value-keyed search would find the wrong tab.
  RecordSubject(browser(), after);

  ForceSessionRebuild(browser()->profile());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRebuildDiscardTest,
                       RebuildAfterReplacementDiscardKeepsValue) {
  tabs::TabInitialUrlHelper* helper = FindRecordedSubject(browser());
  ASSERT_NE(nullptr, helper)
      << "the rebuild lost the value of a discard-replaced tab";
  EXPECT_EQ(GURL("https://edited.test/"), helper->initial_url());
  EXPECT_TRUE(helper->is_user_locked());
}


// ---------------------------------------------------------------------------
// The restored-but-UNLOADED case (the worst one for the user: a window restored
// and left alone until the next quit). A restored tab is armed with its value
// and lock immediately, but its session write is DEFERRED to the first committed
// navigation — so nothing has announced that value, and a rebuild that re-derives
// the file from live state must read the helper to keep it.
//
// Not-loading is enforced by construction: the subject's page is served by a
// ControllableHttpResponse that the middle process NEVER completes, so its
// restore load can start but can never commit. (A TestNavigationObserver would
// only watch a commit, not prevent one.) Because a commit is impossible, the
// commit counter's attach timing cannot change the outcome; it is there to prove
// the condition held all the way to shutdown rather than to create it.
// ---------------------------------------------------------------------------
class CommittedNavigationCounter : public content::WebContentsObserver {
 public:
  explicit CommittedNavigationCounter(content::WebContents* contents)
      : content::WebContentsObserver(contents) {}

  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && handle->HasCommitted()) {
      ++commits_;
    }
  }

  int commits() const { return commits_; }

 private:
  int commits_ = 0;
};

class RoamuxInitialUrlUnloadedRestoreTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxInitialUrlUnloadedRestoreTest() {
    features_.InitAndEnableFeature(features::kInitialUrl);
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    // Registered BEFORE the server starts. Each process gets its own instance:
    // the seeding process completes it (so the page commits and the tab is a
    // real restorable tab); the middle process never touches it.
    hung_ = std::make_unique<net::test_server::ControllableHttpResponse>(
        embedded_test_server(), kSubjectPath);
    ASSERT_TRUE(embedded_test_server()->Start());
    SessionStartupPref::SetStartupPref(
        browser()->profile(), SessionStartupPref(SessionStartupPref::LAST));
    InProcessBrowserTest::SetUpOnMainThread();
  }

 protected:
  static constexpr char kSubjectPath[] = "/roam320-subject";
  // The subject is located by its URL — an identifier independent of the value
  // under test, which is a different URL entirely.
  content::WebContents* FindSubject() {
    TabStripModel* tab_strip = browser()->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* contents = tab_strip->GetWebContentsAt(i);
      if (contents->GetVisibleURL().path() == kSubjectPath) {
        return contents;
      }
    }
    return nullptr;
  }

  std::unique_ptr<net::test_server::ControllableHttpResponse> hung_;
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlUnloadedRestoreTest,
                       PRE_PRE_RestoredButUnloadedTabSurvivesRebuild) {
  // Seed: a background tab on the controllable page, completed here so it
  // commits and becomes a restorable tab; then an EDIT gives it a locked value
  // distinct from its own URL.
  // Must NOT wait for the load here: the response is only completed below, so
  // waiting would deadlock the seeding process.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), embedded_test_server()->GetURL(kSubjectPath),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_TAB);
  hung_->WaitForRequest();
  hung_->Send(net::HTTP_OK, "text/html", "<html><body>subject</body></html>");
  hung_->Done();

  content::WebContents* subject = FindSubject();
  ASSERT_NE(nullptr, subject);
  ASSERT_TRUE(content::WaitForLoadStop(subject));
  ASSERT_TRUE(tabs::SubmitEditInitialUrlForTesting(
      subject, "https://unloaded-subject.test/"));
  ASSERT_TRUE(
      tabs::TabInitialUrlHelper::FromWebContents(subject)->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlUnloadedRestoreTest,
                       PRE_RestoredButUnloadedTabSurvivesRebuild) {
  // The session was restored. This process NEVER completes the controllable
  // response, so the subject cannot commit a navigation.
  content::WebContents* subject = FindSubject();
  ASSERT_NE(nullptr, subject) << "the subject tab was not restored";
  CommittedNavigationCounter counter(subject);

  tabs::TabInitialUrlHelper* helper =
      tabs::TabInitialUrlHelper::FromWebContents(subject);
  ASSERT_NE(nullptr, helper);
  // Armed by the restore, with the deferred write still pending.
  ASSERT_EQ(GURL("https://unloaded-subject.test/"), helper->initial_url());
  ASSERT_TRUE(helper->is_user_locked());
  ASSERT_EQ(0, counter.commits());

  ForceSessionRebuild(browser()->profile());

  // Still no commit: had one occurred, the deferred write would have
  // re-persisted the value and repaired the file, and this test would pass on
  // the unpatched tree for the wrong reason.
  EXPECT_EQ(0, counter.commits());
  EXPECT_EQ(GURL("https://unloaded-subject.test/"), helper->initial_url());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlUnloadedRestoreTest,
                       RestoredButUnloadedTabSurvivesRebuild) {
  content::WebContents* subject = FindSubject();
  ASSERT_NE(nullptr, subject) << "the subject tab was not restored";
  tabs::TabInitialUrlHelper* helper =
      tabs::TabInitialUrlHelper::FromWebContents(subject);
  ASSERT_NE(nullptr, helper)
      << "the rebuild erased the value of a restored, never-navigated tab";
  EXPECT_TRUE(helper->has_initial_url());
  EXPECT_EQ(GURL("https://unloaded-subject.test/"), helper->initial_url());
  EXPECT_TRUE(helper->is_user_locked());
}

}  // namespace
}  // namespace roamux
