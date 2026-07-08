// SPDX-License-Identifier: Apache-2.0
// roam-14 (I-2.5, §4.7): full-restart SessionRestore of an edited (locked)
// initial URL — the PRODUCER path (capture/edit writes via
// SessionService::AddTabExtraData), restored across an in-harness restart.

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "roamex/browser/tabs/tab_initial_url_helper.h"
#include "roamex/browser/ui/tabs/edit_initial_url_dialog.h"
#include "roamex/common/roamex_features.h"

namespace roamex {
namespace {

// SessionRestore requires a persistent profile; PRE_ seeds it, the twin reads.
class RoamexInitialUrlRestoreTest : public InProcessBrowserTest {
 public:
  RoamexInitialUrlRestoreTest() {
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

IN_PROC_BROWSER_TEST_F(RoamexInitialUrlRestoreTest,
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

IN_PROC_BROWSER_TEST_F(RoamexInitialUrlRestoreTest,
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

}  // namespace
}  // namespace roamex
