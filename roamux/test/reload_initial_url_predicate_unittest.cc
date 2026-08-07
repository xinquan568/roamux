// SPDX-License-Identifier: Apache-2.0
// roam-268: the per-contents initial-URL predicate lifted out of
// CanReloadInitialUrl (plan §3.3). The lift must be behaviour-preserving, so
// the core of this suite is an AGREEMENT matrix: for the active tab, the lifted
// predicate and the Browser overload must return the same answer in every
// state. The non-active-tab case is the capability the lift exists to add.
// (TDD: written RED before the definition — the declaration lands first, so
// this suite fails at link until roam-268's implementation exists.)

#include "roamux/browser/tabs/reload_initial_url_command.h"

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/common/roamux_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux::tabs {
namespace {

constexpr char kInitial[] = "https://example.test/start";
constexpr char kOther[] = "https://example.test/elsewhere";
// The roam-11 ignorable start: a tab opened here captures NO initial URL, which
// is how this suite builds the "no initial URL" state. Navigating a tab to a
// real URL would capture it (patch 0009 attaches the helper to every tab when
// kInitialUrl is on), so `kOther` is never the way to get an empty tab.
constexpr char kBlank[] = "about:blank";

class ReloadInitialUrlPredicateTest : public BrowserWithTestWindowTest {
 public:
  void SetUp() override {
    // kTabVisitNav must be pinned OFF here. It ships enabled, and patch 0020
    // builds a TabVisitGestureWatcher in BrowserWindowFeatures::
    // InitPostWindowConstruction; that watcher calls
    // views::EventMonitor::CreateWindowMonitor, which CHECKs on a non-null
    // native window. BrowserWithTestWindowTest's TestBrowserWindow has none, so
    // leaving the flag at its default aborts every test in this fixture during
    // Browser construction — before any test body runs.
    features_.InitWithFeatures(/*enabled=*/{features::kInitialUrl},
                               /*disabled=*/{features::kTabVisitNav});
    BrowserWithTestWindowTest::SetUp();
  }

 protected:
  // Adds a tab and attaches the roam-11 helper to it, returning its contents.
  content::WebContents* AddTabWithHelper(const GURL& url) {
    AddTab(browser(), url);
    content::WebContents* contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    TabInitialUrlHelper::MaybeCreateForWebContents(contents);
    return contents;
  }

  TabInitialUrlHelper* HelperFor(content::WebContents* contents) {
    return TabInitialUrlHelper::FromWebContents(contents);
  }

  base::test::ScopedFeatureList features_;
};

// --- The agreement matrix: lifted predicate == Browser overload, active tab.

TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenTabHasNoInitialUrl) {
  content::WebContents* contents = AddTabWithHelper(GURL(kBlank));
  ASSERT_TRUE(HelperFor(contents));
  ASSERT_FALSE(HelperFor(contents)->has_initial_url());

  EXPECT_FALSE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

// The plain CAPTURED state: no SetUser/SetRestored call, just the roam-11
// capture rule running on the tab's first user-intended navigation. This is the
// state the refresh-all run will meet most often in the wild.
TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenInitialUrlWasCaptured) {
  content::WebContents* contents = AddTabWithHelper(GURL(kOther));
  ASSERT_TRUE(HelperFor(contents)->has_initial_url())
      << "patch 0009 attaches the helper to every tab, so a first navigation "
         "to a non-ignorable URL captures it";
  EXPECT_EQ(GURL(kOther), HelperFor(contents)->initial_url());

  EXPECT_TRUE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenInitialUrlIsUserSetAndLocked) {
  content::WebContents* contents = AddTabWithHelper(GURL(kOther));
  HelperFor(contents)->SetUserInitialUrl(GURL(kInitial));

  EXPECT_TRUE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenInitialUrlIsRestored) {
  content::WebContents* contents = AddTabWithHelper(GURL(kOther));
  HelperFor(contents)->SetRestoredInitialUrl(GURL(kInitial));

  EXPECT_TRUE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenInitialUrlIsInvalid) {
  content::WebContents* contents = AddTabWithHelper(GURL(kOther));
  HelperFor(contents)->SetUserInitialUrl(GURL("not a url"));

  EXPECT_FALSE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

TEST_F(ReloadInitialUrlPredicateTest, AgreesWhenFlagIsOff) {
  content::WebContents* contents = AddTabWithHelper(GURL(kOther));
  HelperFor(contents)->SetUserInitialUrl(GURL(kInitial));
  ASSERT_TRUE(CanReloadInitialUrlForContents(contents));

  // The flag gate must live in the LIFTED predicate, so that both entry points
  // gate identically — this is the way a "mechanical" lift can silently change
  // when Ctrl+Cmd+R is enabled.
  base::test::ScopedFeatureList flag_off;
  flag_off.InitAndDisableFeature(features::kInitialUrl);

  EXPECT_FALSE(CanReloadInitialUrlForContents(contents));
  EXPECT_EQ(CanReloadInitialUrl(browser()),
            CanReloadInitialUrlForContents(contents));
}

// --- The capability the lift exists to add: a NON-active tab.

TEST_F(ReloadInitialUrlPredicateTest, AnswersForNonActiveTabs) {
  content::WebContents* first = AddTabWithHelper(GURL(kOther));
  HelperFor(first)->SetUserInitialUrl(GURL(kInitial));

  // A second tab, which becomes the active one, with no initial URL.
  content::WebContents* second = AddTabWithHelper(GURL(kBlank));
  ASSERT_NE(first, second);
  ASSERT_EQ(second, browser()->tab_strip_model()->GetActiveWebContents());

  // The Browser overload can only see the active tab...
  EXPECT_FALSE(CanReloadInitialUrl(browser()));
  EXPECT_FALSE(CanReloadInitialUrlForContents(second));
  // ...while the lifted predicate answers for the background tab too. This is
  // the whole reason for the lift (roam-269 evaluates it at dequeue).
  EXPECT_TRUE(CanReloadInitialUrlForContents(first));
}

// --- Null tolerance, preserved on both entry points.

TEST_F(ReloadInitialUrlPredicateTest, NullIsFalseOnBothEntryPoints) {
  EXPECT_FALSE(CanReloadInitialUrlForContents(nullptr));
  EXPECT_FALSE(CanReloadInitialUrl(nullptr));
}

}  // namespace
}  // namespace roamux::tabs
