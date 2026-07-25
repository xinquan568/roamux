// SPDX-License-Identifier: Apache-2.0
// roam-220: the side-panel (WebUI) path for "Open N subfolders as tab groups
// in new window". These tests pin the shared browser-side seam both menu
// surfaces now use: GetQualifyingSubfolderCountForMenus (row visibility +
// label N, all gates in one place) and ValidateAndOpenSubfolderGroups (the
// renderer-hardened execute path: re-resolves and revalidates every gate and
// rebuilds plans immediately before opening — a mojo message is untrusted
// input, so a stale or non-qualifying request must be a safe no-op).

#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/test/bookmark_test_helpers.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "roamux/browser/bookmarks/subfolder_tab_groups.h"
#include "roamux/common/roamux_features.h"
#include "url/gurl.h"

namespace roamux {
namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;

class RoamuxSubfolderSidePanelTest : public InProcessBrowserTest {
public:
  RoamuxSubfolderSidePanelTest() {
    features_.InitAndEnableFeature(features::kBookmarkSubfolderGroups);
  }

protected:
  BookmarkModel *model() {
    return BookmarkModelFactory::GetForBrowserContext(browser()->profile());
  }

  // other_bookmarks/Parent/{Sub1/{a}, Sub2/{b,c}, Empty/, direct-link}
  const BookmarkNode *BuildQualifyingTree() {
    BookmarkModel *m = model();
    bookmarks::test::WaitForBookmarkModelToLoad(m);
    const BookmarkNode *parent = m->AddFolder(m->other_node(), 0, u"Parent");
    const BookmarkNode *sub1 = m->AddFolder(parent, 0, u"Sub1");
    m->AddURL(sub1, 0, u"a", GURL("https://a.example/"));
    const BookmarkNode *sub2 = m->AddFolder(parent, 1, u"Sub2");
    m->AddURL(sub2, 0, u"b", GURL("https://b.example/"));
    m->AddURL(sub2, 1, u"c", GURL("https://c.example/"));
    m->AddFolder(parent, 2, u"Empty");
    m->AddURL(parent, 3, u"direct", GURL("https://d.example/"));
    return parent;
  }

  base::test::ScopedFeatureList features_;
};

class RoamuxSubfolderSidePanelFlagOffTest
    : public RoamuxSubfolderSidePanelTest {
public:
  RoamuxSubfolderSidePanelFlagOffTest() {
    features_.Reset();
    features_.InitAndDisableFeature(features::kBookmarkSubfolderGroups);
  }
};

// Count: a qualifying tree reports one plan per non-empty immediate subfolder.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, CountQualifyingTree) {
  const BookmarkNode *parent = BuildQualifyingTree();
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(), parent),
            2);
}

// Count gates: flag off → 0 even for a qualifying tree.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelFlagOffTest, CountZeroFlagOff) {
  const BookmarkNode *parent = BuildQualifyingTree();
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(), parent),
            0);
}

// Count gates: OTR profile → 0.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, CountZeroOffTheRecord) {
  const BookmarkNode *parent = BuildQualifyingTree();
  Browser *otr = CreateIncognitoBrowser();
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(otr->profile(), parent), 0);
}

// Count gates: Incognito mode FORCED → 0 (window/group spray is confined).
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, CountZeroIncognitoForced) {
  const BookmarkNode *parent = BuildQualifyingTree();
  browser()->profile()->GetPrefs()->SetInteger(
      policy::policy_prefs::kIncognitoModeAvailability,
      static_cast<int>(policy::IncognitoModeAvailability::kForced));
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(), parent),
            0);
}

// Count gates: permanent folders and URL nodes are never qualifying anchors.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, CountZeroWrongNodeKinds) {
  BookmarkModel *m = model();
  bookmarks::test::WaitForBookmarkModelToLoad(m);
  const BookmarkNode *url_node =
      m->AddURL(m->other_node(), 0, u"leaf", GURL("https://l.example/"));
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(),
                                                m->other_node()),
            0);
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(), url_node),
            0);
  EXPECT_EQ(GetQualifyingSubfolderCountForMenus(browser()->profile(), nullptr),
            0);
}

// Execute: opens one new window with one named group per qualifying subfolder.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, ExecuteOpensGroups) {
  const BookmarkNode *parent = BuildQualifyingTree();
  const size_t browsers_before = chrome::GetTotalBrowserCount();

  ui_test_utils::BrowserCreatedObserver observer;
  EXPECT_TRUE(ValidateAndOpenSubfolderGroups(browser(), parent));
  Browser *opened = observer.Wait();

  EXPECT_EQ(chrome::GetTotalBrowserCount(), browsers_before + 1);
  ASSERT_NE(opened, browser());
  EXPECT_EQ(opened->tab_strip_model()->group_model()->ListTabGroups().size(),
            2u);
}

// Execute hardening: gates rechecked at execute time — a request that stopped
// qualifying between query and execute is a safe no-op.
IN_PROC_BROWSER_TEST_F(RoamuxSubfolderSidePanelTest, ExecuteRevalidates) {
  BookmarkModel *m = model();
  const BookmarkNode *parent = BuildQualifyingTree();
  // The renderer saw count==2, then the user emptied the folder.
  while (!parent->children().empty()) {
    m->Remove(parent->children()[0].get(),
              bookmarks::metrics::BookmarkEditSource::kOther, FROM_HERE);
  }
  const size_t browsers_before = chrome::GetTotalBrowserCount();
  EXPECT_FALSE(ValidateAndOpenSubfolderGroups(browser(), parent));
  EXPECT_EQ(chrome::GetTotalBrowserCount(), browsers_before);
  // And the wrong-kind / null anchors are equally inert.
  EXPECT_FALSE(ValidateAndOpenSubfolderGroups(browser(), m->other_node()));
  EXPECT_FALSE(ValidateAndOpenSubfolderGroups(browser(), nullptr));
}

} // namespace
} // namespace roamux
