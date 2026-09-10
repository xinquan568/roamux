// SPDX-License-Identifier: Apache-2.0
// roam-10 (I-2.1) mandatory matrix (plan D6): live uniqueness, duplicate
// mints fresh, TabRestoreService reopen reuses through the real hand-off,
// crafted-collision re-stamps, flag-off inert, OTR isolated and unpersisted.
// (TDD: written RED against patch 0009.)

#include <map>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/threading/thread_restrictions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/sessions/core/session_id.h"
#include "net/dns/mock_host_resolver.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_utils.h"
#include "roamux/browser/tabs/tab_uid_service.h"
#include "roamux/browser/tabs/tab_uid_service_factory.h"
#include "roamux/browser/tabs/tab_uid_tab_helper.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

class RoamuxTabUidTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxTabUidTest() { features_.InitAndEnableFeature(features::kInitialUrl); }

 protected:
  tabs::TabUidService* service() {
    return tabs::TabUidServiceFactory::GetForProfile(browser()->profile());
  }

  std::string UidOfTabAt(int index) {
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetWebContentsAt(index);
    tabs::TabUidTabHelper* helper =
        tabs::TabUidTabHelper::FromWebContents(web_contents);
    return helper ? helper->uid() : std::string();
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, EveryLiveTabHasADistinctUid) {
  ASSERT_TRUE(AddTabAtIndex(1, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));
  ASSERT_TRUE(AddTabAtIndex(2, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));
  const std::string uid0 = UidOfTabAt(0);
  const std::string uid1 = UidOfTabAt(1);
  const std::string uid2 = UidOfTabAt(2);
  EXPECT_FALSE(uid0.empty());
  EXPECT_FALSE(uid1.empty());
  EXPECT_FALSE(uid2.empty());
  EXPECT_NE(uid0, uid1);
  EXPECT_NE(uid1, uid2);
  EXPECT_NE(uid0, uid2);
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, DuplicateMintsFresh) {
  const std::string original = UidOfTabAt(0);
  ASSERT_FALSE(original.empty());
  chrome::DuplicateTab(browser());
  ASSERT_EQ(2, browser()->tab_strip_model()->count());
  const std::string duplicate = UidOfTabAt(1);
  EXPECT_FALSE(duplicate.empty());
  EXPECT_NE(original, duplicate);
  EXPECT_TRUE(service()->IsLive(original));
  EXPECT_TRUE(service()->IsLive(duplicate));
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, ReopenReusesTheClosedTabsUid) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  ASSERT_TRUE(AddTabAtIndex(1, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));
  const std::string uid = UidOfTabAt(1);
  ASSERT_FALSE(uid.empty());

  content::WebContentsDestroyedWatcher destroyed(
      browser()->tab_strip_model()->GetWebContentsAt(1));
  browser()->tab_strip_model()->CloseWebContentsAt(
      1, TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  destroyed.Wait();
  EXPECT_FALSE(service()->IsLive(uid));

  // Reopen via TabRestoreService — flows through the real 0009 hand-off.
  chrome::RestoreTab(browser());
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(2, browser()->tab_strip_model()->count());
  EXPECT_EQ(uid, UidOfTabAt(1));
  EXPECT_TRUE(service()->IsLive(uid));
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, LiveCollisionRestamps) {
  const std::string live_uid = UidOfTabAt(0);
  ASSERT_FALSE(live_uid.empty());

  // Craft a "restored" tab carrying a uid that is ALREADY live (§6.9 startup
  // collision), driving the real hand-off + attach path.
  content::WebContents::CreateParams create_params(browser()->profile());
  std::unique_ptr<content::WebContents> web_contents =
      content::WebContents::Create(create_params);
  tabs::TabUidTabHelper::SetPendingRestoredUid(
      web_contents.get(), {{tabs::TabUidTabHelper::kExtraDataKey, live_uid}});
  browser()->tab_strip_model()->AppendWebContents(std::move(web_contents),
                                                  /*foreground=*/true);
  base::RunLoop().RunUntilIdle();

  const std::string restamped = UidOfTabAt(1);
  EXPECT_FALSE(restamped.empty());
  EXPECT_NE(live_uid, restamped);
  EXPECT_EQ(live_uid, UidOfTabAt(0)) << "the incumbent keeps its identity";
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, OtrIsIsolatedAndUnpersisted) {
  Browser* incognito = CreateIncognitoBrowser();
  tabs::TabUidService* otr_service =
      tabs::TabUidServiceFactory::GetForProfile(incognito->profile());
  ASSERT_NE(nullptr, otr_service);
  EXPECT_NE(service(), otr_service) << "OTR must not redirect (D5)";

  content::WebContents* otr_contents =
      incognito->tab_strip_model()->GetWebContentsAt(0);
  tabs::TabUidTabHelper* helper =
      tabs::TabUidTabHelper::FromWebContents(otr_contents);
  ASSERT_NE(nullptr, helper);
  EXPECT_FALSE(helper->uid().empty());
  EXPECT_TRUE(otr_service->IsLive(helper->uid()));
  EXPECT_FALSE(service()->IsLive(helper->uid()))
      << "OTR identities never enter the regular registry";
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidTest, DiscardKeepsTheSameUid) {
  // A background tab (the active tab cannot be discarded).
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  ASSERT_EQ(2, browser()->tab_strip_model()->count());
  const std::string uid = UidOfTabAt(1);
  ASSERT_FALSE(uid.empty());

  // Drive the canonical discard flow — the same live tab gets replacement
  // contents; identity must survive without a re-stamp.
  TabListInterface* tab_list = TabListInterface::From(browser());
  ASSERT_NE(nullptr, tab_list);
  ASSERT_NE(nullptr, tab_list->DiscardTab(tab_list->GetTab(1)->GetHandle()));
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(uid, UidOfTabAt(1));
  EXPECT_TRUE(service()->IsLive(uid));
}

class RoamuxTabUidFlagOffTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxTabUidFlagOffTest() {
    // The uid helper is minted when EITHER kInitialUrl OR kTabVisitNav is on
    // (tab_uid_tab_helper.cc). Both ship default-on (roam-187, roam-189), so
    // "no helper" requires pinning BOTH off — disabling only kInitialUrl
    // stopped proving inertness the day kTabVisitNav graduated.
    features_.InitWithFeatures(
        /*enabled=*/{},
        /*disabled=*/{features::kInitialUrl, features::kTabVisitNav});
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxTabUidFlagOffTest, NoHelperNoUidWhenFlagOff) {
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetWebContentsAt(0);
  EXPECT_EQ(nullptr, tabs::TabUidTabHelper::FromWebContents(web_contents));
}


void ForceSessionRebuildForUidTest(Profile* profile) {
  SessionService* session_service = SessionServiceFactory::GetForProfile(profile);
  ASSERT_TRUE(session_service);
  session_service->ResetFromCurrentBrowsers();
}

// ---------------------------------------------------------------------------
// roam-320: a session-log rebuild must not re-mint the durable uid.
//
// The uid rides the same append-only "extra data" command as the initial URL,
// and the rebuild emits none, so on relaunch AdoptOrRestamp() sees no restored
// value and mints a FRESH uid — breaking the identity E4's tab-visit navigation
// keys are built on.
//
// Two hazards this fixture is shaped around. First, the uid helper stamps from
// its CONSTRUCTOR, when the tab's window id is still invalid, so that write is
// dropped by SessionService's tracking guard: a naive "the uid changed" failure
// could therefore mean the uid was never persisted rather than that the rebuild
// erased it. So the test first establishes that a uid command exists under the
// ATTACHED tab's id, by re-persisting through the normal producer path and only
// then forcing the rebuild. Second, the expected value must cross processes
// durably: it is written under the PRE_-shared user-data directory, not a
// per-process temp dir and not a member.
// ---------------------------------------------------------------------------
class RoamuxTabUidRebuildTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxTabUidRebuildTest() {
    features_.InitAndEnableFeature(features::kInitialUrl);
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    SessionStartupPref::SetStartupPref(
        browser()->profile(), SessionStartupPref(SessionStartupPref::LAST));
    InProcessBrowserTest::SetUpOnMainThread();
  }

 protected:
  // The profile directory survives between a PRE_ test and its twin; a process
  // temp dir does not.
  base::FilePath ExpectedUidPath() {
    return browser()->profile()->GetPath().AppendASCII("roam320-expected-uid");
  }

  void WriteExpectedUid(const std::string& uid) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(ExpectedUidPath(), uid));
  }

  std::string ReadExpectedUid() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string uid;
    EXPECT_TRUE(base::ReadFileToString(ExpectedUidPath(), &uid));
    return uid;
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxTabUidRebuildTest, PRE_RebuildKeepsTabUid) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  tabs::TabUidTabHelper* helper =
      tabs::TabUidTabHelper::FromWebContents(contents);
  ASSERT_NE(nullptr, helper);
  const std::string uid = helper->uid();
  ASSERT_FALSE(uid.empty());
  WriteExpectedUid(uid);

  // Establish a uid command under the ATTACHED tab's id: the constructor stamp
  // ran while the window id was invalid and was dropped by the tracking guard,
  // so without this the post-restart assertion would not be about the rebuild.
  SessionService* session_service =
      SessionServiceFactory::GetForProfile(browser()->profile());
  ASSERT_TRUE(session_service);
  const SessionID window_id =
      sessions::SessionTabHelper::IdForWindowContainingTab(contents);
  const SessionID tab_id = sessions::SessionTabHelper::IdForTab(contents);
  ASSERT_TRUE(window_id.is_valid());
  ASSERT_TRUE(tab_id.is_valid());
  session_service->AddTabExtraData(
      window_id, tab_id, tabs::TabUidTabHelper::kExtraDataKey, uid);

  ForceSessionRebuildForUidTest(browser()->profile());
}

IN_PROC_BROWSER_TEST_F(RoamuxTabUidRebuildTest, RebuildKeepsTabUid) {
  TabStripModel* tab_strip = browser()->tab_strip_model();
  const std::string expected = ReadExpectedUid();
  ASSERT_FALSE(expected.empty());

  bool found = false;
  for (int i = 0; i < tab_strip->count(); ++i) {
    ASSERT_TRUE(content::WaitForLoadStop(tab_strip->GetWebContentsAt(i)));
    tabs::TabUidTabHelper* helper =
        tabs::TabUidTabHelper::FromWebContents(tab_strip->GetWebContentsAt(i));
    if (helper && helper->uid() == expected) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found)
      << "the rebuild erased the uid; the restored tab was re-minted";
}

}  // namespace
}  // namespace roamux
