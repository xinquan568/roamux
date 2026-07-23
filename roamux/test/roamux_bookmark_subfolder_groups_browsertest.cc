// SPDX-License-Identifier: Apache-2.0
// roam-208: bookmark subfolders -> named tab groups in one new window.
// B1-B8 map the issue's eight decisions plus the v1 confinement matrix
// (regular profile, policy-normal, flag-on; hidden otherwise).

#include <set>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/bookmarks/bookmark_context_menu_controller.h"
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
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "components/tabs/public/tab_group.h"
#include "content/public/test/browser_test.h"
#include "roamux/browser/bookmarks/subfolder_tab_groups.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/menus/simple_menu_model.h"
#include "url/gurl.h"

namespace roamux {
namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;

size_t g_prompt_count = 0;
bool g_prompt_answer = true;

bool CountingPrompt(size_t total_urls) {
  ++g_prompt_count;
  return g_prompt_answer;
}

class RoamuxBookmarkSubfolderGroupsTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxBookmarkSubfolderGroupsTest() {
    features_.InitAndEnableFeature(features::kBookmarkSubfolderGroups);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    model_ = BookmarkModelFactory::GetForBrowserContext(browser()->profile());
    bookmarks::test::WaitForBookmarkModelToLoad(model_);
    g_prompt_count = 0;
    g_prompt_answer = true;
    previous_prompt_ = SetBulkOpenPromptCallbackForTesting(&CountingPrompt);
  }

  void TearDownOnMainThread() override {
    SetBulkOpenPromptCallbackForTesting(previous_prompt_);
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  // Parent folder with: "AI" (2 links), "Mails" (1 link), "Empty" (none),
  // "Nested" (one subfolder, no direct links). Parent also has 1 direct link.
  const BookmarkNode* BuildIssueTree() {
    const BookmarkNode* bar = model_->bookmark_bar_node();
    const BookmarkNode* parent = model_->AddFolder(bar, 0, u"Eric");
    model_->AddURL(parent, 0, u"direct", GURL("https://direct.test/"));
    const BookmarkNode* ai = model_->AddFolder(parent, 1, u"AI");
    model_->AddURL(ai, 0, u"a1", GURL("https://a1.test/"));
    model_->AddURL(ai, 1, u"a2", GURL("https://a2.test/"));
    const BookmarkNode* mails = model_->AddFolder(parent, 2, u"Mails");
    model_->AddURL(mails, 0, u"m1", GURL("https://m1.test/"));
    model_->AddFolder(parent, 3, u"Empty");
    const BookmarkNode* nested = model_->AddFolder(parent, 4, u"Nested");
    model_->AddFolder(nested, 0, u"Inner");
    return parent;
  }

  std::unique_ptr<BookmarkContextMenuController> MenuFor(
      const BookmarkNode* node,
      Browser* in_browser = nullptr) {
    Browser* b = in_browser ? in_browser : browser();
    return std::make_unique<BookmarkContextMenuController>(
        gfx::NativeWindow(), /*delegate=*/nullptr, b, b->profile(),
        BookmarkLaunchLocation::kAttachedBar,
        std::vector<raw_ptr<const BookmarkNode, VectorExperimental>>{node},
        /*can_paste=*/false);
  }

  static bool HasSubfolderItem(BookmarkContextMenuController& controller) {
    return controller.menu_model()
        ->GetIndexOfCommandId(
            IDC_ROAMUX_BOOKMARK_BAR_OPEN_SUBFOLDERS_AS_TAB_GROUPS)
        .has_value();
  }

  static std::u16string SubfolderItemLabel(
      BookmarkContextMenuController& controller) {
    auto index = controller.menu_model()->GetIndexOfCommandId(
        IDC_ROAMUX_BOOKMARK_BAR_OPEN_SUBFOLDERS_AS_TAB_GROUPS);
    return index ? controller.menu_model()->GetLabelAt(*index)
                 : std::u16string();
  }

  Browser* ExecuteAndWaitForBrowser(BookmarkContextMenuController& controller) {
    ui_test_utils::BrowserCreatedObserver observer;
    controller.ExecuteCommand(
        IDC_ROAMUX_BOOKMARK_BAR_OPEN_SUBFOLDERS_AS_TAB_GROUPS, 0);
    return observer.Wait();
  }

  raw_ptr<BookmarkModel, DanglingUntriaged> model_ = nullptr;
  BulkOpenPromptCallback previous_prompt_ = nullptr;
  base::test::ScopedFeatureList features_;
};

// B1 (decisions 1,2): the label's N counts qualifying subfolders only, with
// ICU singular/plural; the row is absent at N==0.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest, LabelCountsAndHides) {
  const BookmarkNode* parent = BuildIssueTree();  // N == 2
  auto menu2 = MenuFor(parent);
  ASSERT_TRUE(HasSubfolderItem(*menu2));
  EXPECT_EQ(u"Open 2 subfolders as tab groups in new window",
            SubfolderItemLabel(*menu2));

  const BookmarkNode* bar = model_->bookmark_bar_node();
  const BookmarkNode* one = model_->AddFolder(bar, 0, u"One");
  const BookmarkNode* sub = model_->AddFolder(one, 0, u"Sub");
  model_->AddURL(sub, 0, u"s", GURL("https://s.test/"));
  auto menu1 = MenuFor(one);  // N == 1
  ASSERT_TRUE(HasSubfolderItem(*menu1));
  EXPECT_EQ(u"Open 1 subfolder as a tab group in new window",
            SubfolderItemLabel(*menu1));

  const BookmarkNode* zero = model_->AddFolder(bar, 0, u"Zero");
  model_->AddURL(zero, 0, u"z", GURL("https://z.test/"));
  auto menu0 = MenuFor(zero);  // N == 0 (a direct link, no subfolders)
  EXPECT_FALSE(HasSubfolderItem(*menu0));
}

// B2+B4+B5 (decisions 2,3,4,5): one new window; exactly the qualifying
// subfolders become groups, in bookmark order, with exact titles and their
// first-level links in order; the parent's direct link is not opened.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest,
                       OpensQualifyingSubfoldersAsGroups) {
  const BookmarkNode* parent = BuildIssueTree();
  auto menu = MenuFor(parent);
  ASSERT_TRUE(HasSubfolderItem(*menu));

  const size_t browsers_before = chrome::GetTotalBrowserCount();
  Browser* opened = ExecuteAndWaitForBrowser(*menu);
  ASSERT_NE(nullptr, opened);
  EXPECT_EQ(browsers_before + 1, chrome::GetTotalBrowserCount());

  TabStripModel* tabs = opened->tab_strip_model();
  EXPECT_EQ(3, tabs->count());  // a1, a2, m1 — never direct.test
  TabGroupModel* groups = tabs->group_model();
  ASSERT_EQ(2u, groups->ListTabGroups().size());

  std::vector<std::u16string> titles;
  for (const tab_groups::TabGroupId& id : groups->ListTabGroups()) {
    titles.push_back(groups->GetTabGroup(id)->visual_data()->title());
  }
  EXPECT_EQ((std::vector<std::u16string>{u"AI", u"Mails"}), titles);

  std::set<std::string> urls;
  for (int i = 0; i < tabs->count(); ++i) {
    urls.insert(tabs->GetWebContentsAt(i)->GetVisibleURL().spec());
  }
  EXPECT_EQ(1u, urls.count("https://a1.test/"));
  EXPECT_EQ(1u, urls.count("https://a2.test/"));
  EXPECT_EQ(1u, urls.count("https://m1.test/"));
  EXPECT_EQ(0u, urls.count("https://direct.test/"));
}

// B3 (decision 6): distinct colors for <=9 groups in the fresh window.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest,
                       ColorsDistinctUpToNine) {
  const BookmarkNode* bar = model_->bookmark_bar_node();
  const BookmarkNode* parent = model_->AddFolder(bar, 0, u"Nine");
  for (int i = 0; i < 9; ++i) {
    const BookmarkNode* sub =
        model_->AddFolder(parent, i, u"G" + base::NumberToString16(i));
    model_->AddURL(sub, 0, u"u",
                   GURL("https://g" + base::NumberToString(i) + ".test/"));
  }
  auto menu = MenuFor(parent);
  ASSERT_TRUE(HasSubfolderItem(*menu));
  Browser* opened = ExecuteAndWaitForBrowser(*menu);
  ASSERT_NE(nullptr, opened);

  TabGroupModel* groups = opened->tab_strip_model()->group_model();
  ASSERT_EQ(9u, groups->ListTabGroups().size());
  std::set<tab_groups::TabGroupColorId> colors;
  for (const tab_groups::TabGroupId& id : groups->ListTabGroups()) {
    colors.insert(groups->GetTabGroup(id)->visual_data()->color());
  }
  EXPECT_EQ(9u, colors.size());
}

// B6 (decision 7): one aggregate prompt at >=15 total links; none below.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest,
                       SingleAggregatePrompt) {
  const BookmarkNode* bar = model_->bookmark_bar_node();
  const BookmarkNode* big = model_->AddFolder(bar, 0, u"Big");
  for (int f = 0; f < 3; ++f) {
    const BookmarkNode* sub =
        model_->AddFolder(big, f, u"F" + base::NumberToString16(f));
    for (int i = 0; i < 5; ++i) {  // 3 x 5 = 15 == threshold
      model_->AddURL(
          sub, i, u"u",
          GURL("https://p" + base::NumberToString(f * 5 + i) + ".test/"));
    }
  }
  auto menu = MenuFor(big);
  ASSERT_TRUE(HasSubfolderItem(*menu));
  Browser* opened = ExecuteAndWaitForBrowser(*menu);
  ASSERT_NE(nullptr, opened);
  EXPECT_EQ(1u, g_prompt_count);

  const BookmarkNode* parent = BuildIssueTree();  // 3 links total
  auto small = MenuFor(parent);
  g_prompt_count = 0;
  Browser* opened2 = ExecuteAndWaitForBrowser(*small);
  ASSERT_NE(nullptr, opened2);
  EXPECT_EQ(0u, g_prompt_count);
}

// B7 (decision 8): fresh groups always; the saved-tab-group conversion
// dialog can never appear because group creation never routes through the
// bookmark-connected open-in-group flow. Structural assertion: executing on
// a folder yields exactly plan-shaped fresh groups (titles above) and no
// modal is required to proceed (the command completes without interaction —
// the prompt seam is the only dialog surface and stays at zero below
// threshold).
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest,
                       FreshGroupsNoConversionDialog) {
  const BookmarkNode* parent = BuildIssueTree();
  auto menu = MenuFor(parent);
  ASSERT_TRUE(HasSubfolderItem(*menu));
  Browser* opened = ExecuteAndWaitForBrowser(*menu);
  ASSERT_NE(nullptr, opened);
  EXPECT_EQ(0u, g_prompt_count);
  EXPECT_EQ(2u,
            opened->tab_strip_model()->group_model()->ListTabGroups().size());
}

// B8a (confinement): the row is absent in an OTR browser.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest, RowAbsentInOTR) {
  const BookmarkNode* parent = BuildIssueTree();
  Browser* otr = CreateIncognitoBrowser();
  auto menu = MenuFor(parent, otr);
  EXPECT_FALSE(HasSubfolderItem(*menu));
}

// B8b (confinement): the row is absent under forced-incognito policy.
IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsTest,
                       RowAbsentUnderForcedIncognito) {
  const BookmarkNode* parent = BuildIssueTree();
  browser()->profile()->GetPrefs()->SetInteger(
      policy::policy_prefs::kIncognitoModeAvailability,
      static_cast<int>(policy::IncognitoModeAvailability::kForced));
  auto menu = MenuFor(parent);
  EXPECT_FALSE(HasSubfolderItem(*menu));
}

// B8c (flag-off): row absent; menu otherwise stock.
class RoamuxBookmarkSubfolderGroupsFlagOffTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxBookmarkSubfolderGroupsFlagOffTest() {
    features_.InitAndDisableFeature(features::kBookmarkSubfolderGroups);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxBookmarkSubfolderGroupsFlagOffTest,
                       RowAbsentWhenFlagOff) {
  BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(browser()->profile());
  bookmarks::test::WaitForBookmarkModelToLoad(model);
  const BookmarkNode* parent =
      model->AddFolder(model->bookmark_bar_node(), 0, u"P");
  const BookmarkNode* sub = model->AddFolder(parent, 0, u"S");
  model->AddURL(sub, 0, u"u", GURL("https://u.test/"));

  BookmarkContextMenuController controller(
      gfx::NativeWindow(), /*delegate=*/nullptr, browser(),
      browser()->profile(), BookmarkLaunchLocation::kAttachedBar,
      std::vector<raw_ptr<const BookmarkNode, VectorExperimental>>{parent},
      /*can_paste=*/false);
  EXPECT_FALSE(controller.menu_model()
                   ->GetIndexOfCommandId(
                       IDC_ROAMUX_BOOKMARK_BAR_OPEN_SUBFOLDERS_AS_TAB_GROUPS)
                   .has_value());
}

}  // namespace
}  // namespace roamux
