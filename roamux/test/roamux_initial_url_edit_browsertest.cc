// SPDX-License-Identifier: Apache-2.0
// roam-14 (I-2.5, §4.7): edit round-trip (menu delegate + dialog seam),
// set-to-current, duplicate inherits value+lock (uid re-mints), and malformed
// persisted data is survived. Session-restore coverage is the PRE_ pair in
// roamux_initial_url_restore_browsertest.cc.

#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "roamux/browser/tabs/refresh_all_initial_urls_command.h"
#include "roamux/browser/tabs/reload_initial_url_command.h"
#include "roamux/browser/tabs/shortcut_registry.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/tabs/tab_uid_tab_helper.h"
#include "roamux/browser/ui/tabs/edit_initial_url_dialog.h"
#include "roamux/browser/ui/tabs/initial_url_menu.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/window_open_disposition.h"
#include "ui/menus/simple_menu_model.h"

namespace roamux {
namespace {

class RoamuxInitialUrlEditTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxInitialUrlEditTest() {
    features_.InitAndEnableFeature(features::kInitialUrl);
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    InProcessBrowserTest::SetUpOnMainThread();
  }

 protected:
  content::WebContents* active_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
  tabs::TabInitialUrlHelper* helper() {
    return tabs::TabInitialUrlHelper::FromWebContents(active_contents());
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest, EditDialogSeamLocksValue) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  // Edit through the real validate+commit seam.
  ASSERT_TRUE(tabs::SubmitEditInitialUrlForTesting(active_contents(),
                                                   "https://edited.test/"));
  ASSERT_TRUE(helper()->has_initial_url());
  EXPECT_EQ(GURL("https://edited.test/"), helper()->initial_url());
  EXPECT_TRUE(helper()->is_user_locked());

  // A later navigation (would-be capture) must not overwrite the locked value.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title2.html")));
  EXPECT_EQ(GURL("https://edited.test/"), helper()->initial_url());

  // An invalid edit is a no-op.
  EXPECT_FALSE(
      tabs::SubmitEditInitialUrlForTesting(active_contents(), "not a url"));
  EXPECT_EQ(GURL("https://edited.test/"), helper()->initial_url());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       DuplicateInheritsValueAndLock) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  ASSERT_TRUE(tabs::SubmitEditInitialUrlForTesting(active_contents(),
                                                   "https://edited.test/"));
  const std::string source_uid =
      tabs::TabUidTabHelper::FromWebContents(active_contents())->uid();

  chrome::DuplicateTab(browser());
  content::WebContents* dupe = active_contents();
  tabs::TabInitialUrlHelper* dupe_helper =
      tabs::TabInitialUrlHelper::FromWebContents(dupe);
  ASSERT_NE(nullptr, dupe_helper);
  // §4.2: value+lock inherited…
  EXPECT_EQ(GURL("https://edited.test/"), dupe_helper->initial_url());
  EXPECT_TRUE(dupe_helper->is_user_locked());
  // …but the durable uid re-mints (roam-10 uniqueness rule).
  EXPECT_NE(source_uid, tabs::TabUidTabHelper::FromWebContents(dupe)->uid());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest, SetToCurrentPageLocks) {
  const GURL page_b = embedded_test_server()->GetURL("/title2.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page_b));
  // The menu "Set initial URL to current page" path: current committed URL
  // through SetUserInitialUrl (locks). Drive the helper as the delegate does.
  tabs::TabInitialUrlHelper::MaybeCreateForWebContents(active_contents());
  helper()->SetUserInitialUrl(active_contents()->GetLastCommittedURL());
  EXPECT_EQ(page_b, helper()->initial_url());
  EXPECT_TRUE(helper()->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest, ReopenClosedRestoresValue) {
  ASSERT_TRUE(AddTabAtIndex(1, embedded_test_server()->GetURL("/title1.html"),
                            ui::PAGE_TRANSITION_TYPED));
  content::WebContents* tab = browser()->tab_strip_model()->GetWebContentsAt(1);
  ASSERT_TRUE(
      tabs::SubmitEditInitialUrlForTesting(tab, "https://edited.test/"));

  content::WebContentsDestroyedWatcher destroyed(tab);
  browser()->tab_strip_model()->CloseWebContentsAt(
      1, TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  destroyed.Wait();

  // Reopen through the real TabRestoreService hand-off (the 0009 channel).
  chrome::RestoreTab(browser());
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(2, browser()->tab_strip_model()->count());
  tabs::TabInitialUrlHelper* reopened =
      tabs::TabInitialUrlHelper::FromWebContents(
          browser()->tab_strip_model()->GetWebContentsAt(1));
  ASSERT_NE(nullptr, reopened);
  EXPECT_EQ(GURL("https://edited.test/"), reopened->initial_url());
  EXPECT_TRUE(reopened->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       MalformedPersistedDataSurvives) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents::CreateParams params(browser()->profile());
  std::unique_ptr<content::WebContents> restored =
      content::WebContents::Create(params);
  content::WebContents* raw = restored.get();
  // Inject garbage extra data through the real consumer: restore must not
  // crash and the tab stays uncaptured.
  tabs::TabInitialUrlHelper::SetPendingRestoredInitialUrl(
      raw, {{tabs::TabInitialUrlHelper::kExtraDataKey, "@@garbage@@"}});
  browser()->tab_strip_model()->AppendWebContents(std::move(restored),
                                                  /*foreground=*/true);
  base::RunLoop().RunUntilIdle();
  tabs::TabInitialUrlHelper* h =
      tabs::TabInitialUrlHelper::FromWebContents(raw);
  if (h) {
    EXPECT_FALSE(h->has_initial_url());
  }
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       SubMenuAppearsAndDrivesActions) {
  // The submenu appears (flag on) and carries the two §4.5 actions, plus
  // roam-270's separator + "Refresh all tabs to initial URLs" while
  // kRefreshAllInitialUrls is on (it ships enabled). Asserted by COMMAND ID
  // rather than by count alone: a bare count would be satisfied by any four
  // rows in any order, and the §4.5 actions must stay at indices 0 and 1 —
  // the "Set to current page" enablement check below indexes by position.
  ui::SimpleMenuModel parent(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> submenu =
      tabs::MaybeAppendInitialUrlSubMenu(&parent, active_contents());
  ASSERT_NE(nullptr, submenu);
  ASSERT_EQ(4u, submenu->GetItemCount());
  EXPECT_EQ(tabs::kEditInitialUrlCommandId, submenu->GetCommandIdAt(0));
  EXPECT_EQ(tabs::kSetInitialUrlToCurrentPageCommandId,
            submenu->GetCommandIdAt(1));
  EXPECT_EQ(ui::MenuModel::TYPE_SEPARATOR, submenu->GetTypeAt(2));
  EXPECT_EQ(tabs::kRefreshAllInitialUrlsCommandId, submenu->GetCommandIdAt(3));

  // On about:blank, "Set to current page" (index 1) is disabled…
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  {
    ui::SimpleMenuModel parent_blank(nullptr);
    std::unique_ptr<ui::SimpleMenuModel> blank_menu =
        tabs::MaybeAppendInitialUrlSubMenu(&parent_blank, active_contents());
    ASSERT_NE(nullptr, blank_menu);
    EXPECT_FALSE(blank_menu->IsEnabledAt(1));
  }

  // …and enabled on a real page, where activating it writes + locks.
  const GURL page = embedded_test_server()->GetURL("/title1.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
  ui::SimpleMenuModel parent_live(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> live_menu =
      tabs::MaybeAppendInitialUrlSubMenu(&parent_live, active_contents());
  ASSERT_NE(nullptr, live_menu);
  EXPECT_TRUE(live_menu->IsEnabledAt(1));
  live_menu->ActivatedAt(1);  // ExecuteCommand(set-to-current) via the model.
  ASSERT_TRUE(helper()->has_initial_url());
  EXPECT_EQ(page, helper()->initial_url());
  EXPECT_TRUE(helper()->is_user_locked());
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       RetainedSubmenuInertAfterTabDestroyed) {
  // roam-204: patch 0012's TabMenuModel retains this submenu for the whole
  // window's life, so the model must stay inert once its tab is gone — both
  // commands disabled, activation a no-op, destruction releasing nothing.
  ASSERT_TRUE(AddTabAtIndex(1, embedded_test_server()->GetURL("/title1.html"),
                            ui::PAGE_TRANSITION_TYPED));
  ASSERT_EQ(2, browser()->tab_strip_model()->count());
  content::WebContents* tab = browser()->tab_strip_model()->GetWebContentsAt(1);

  // Retain the submenu the way the patched TabMenuModel::Build does.
  ui::SimpleMenuModel parent(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> submenu =
      tabs::MaybeAppendInitialUrlSubMenu(&parent, tab);
  ASSERT_NE(nullptr, submenu);

  // Resolve item indices by command id, not position.
  size_t edit_index = submenu->GetItemCount();
  size_t set_current_index = submenu->GetItemCount();
  for (size_t i = 0; i < submenu->GetItemCount(); ++i) {
    if (submenu->GetCommandIdAt(i) == tabs::kEditInitialUrlCommandId) {
      edit_index = i;
    } else if (submenu->GetCommandIdAt(i) ==
               tabs::kSetInitialUrlToCurrentPageCommandId) {
      set_current_index = i;
    }
  }
  ASSERT_LT(edit_index, submenu->GetItemCount());
  ASSERT_LT(set_current_index, submenu->GetItemCount());

  // Live tab: both commands available.
  ASSERT_TRUE(submenu->IsEnabledAt(edit_index));
  ASSERT_TRUE(submenu->IsEnabledAt(set_current_index));

  content::WebContentsDestroyedWatcher destroyed(tab);
  browser()->tab_strip_model()->CloseWebContentsAt(
      1, TabCloseTypes::CLOSE_USER_GESTURE);
  destroyed.Wait();

  // The lifetime property: a retained submenu whose tab died reports both
  // commands disabled and executes them as no-ops.
  EXPECT_FALSE(submenu->IsEnabledAt(edit_index));
  EXPECT_FALSE(submenu->IsEnabledAt(set_current_index));
  submenu->ActivatedAt(edit_index);         // No dialog, no helper.
  submenu->ActivatedAt(set_current_index);  // No crash, no write.
}

class RoamuxInitialUrlEditFlagOffTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxInitialUrlEditFlagOffTest() {
    features_.InitAndDisableFeature(features::kInitialUrl);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditFlagOffTest, NoHelperWhenFlagOff) {
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  // The set-to-current/edit seams no-op: no helper is created.
  EXPECT_FALSE(
      tabs::SubmitEditInitialUrlForTesting(contents, "https://edited.test/"));
  EXPECT_EQ(nullptr, tabs::TabInitialUrlHelper::FromWebContents(contents));
}

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditFlagOffTest, NoSubmenuWhenFlagOff) {
  ui::SimpleMenuModel parent(nullptr);
  EXPECT_EQ(nullptr,
            tabs::MaybeAppendInitialUrlSubMenu(
                &parent, browser()->tab_strip_model()->GetActiveWebContents()));
}

}  // namespace

// --- roam-270: the refresh-all item -----------------------------------------
//
// Each case below is specified by the defect it must FAIL against. A Step-5
// review rejected the first draft of all three because every one would have
// passed against the bug it was named for.

// DEFECT IT EXCLUDES: a clicked-tab-only predicate. The model holds exactly one
// WebContents, so "is THIS tab eligible" is the natural wrong implementation
// and is indistinguishable from the correct one unless the owning tab is
// ineligible while another tab is not.
IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       RefreshAllEnabledByAnyTabInWindowNotTheClickedTab) {
  // The owning tab is an ignorable start: no initial URL, so INELIGIBLE.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  content::WebContents* owner = active_contents();
  ASSERT_FALSE(tabs::CanReloadInitialUrlForContents(owner));

  {
    ui::SimpleMenuModel parent(nullptr);
    std::unique_ptr<ui::SimpleMenuModel> menu =
        tabs::MaybeAppendInitialUrlSubMenu(&parent, owner);
    ASSERT_NE(nullptr, menu);
    EXPECT_FALSE(menu->IsEnabledAt(3))
        << "enabled with no eligible tab anywhere in the window";
  }

  // Make a DIFFERENT tab eligible; the owning tab stays ineligible.
  ASSERT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("https://example.test/start"),
      WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
  content::WebContents* other =
      browser()->tab_strip_model()->GetWebContentsAt(1);
  tabs::TabInitialUrlHelper::MaybeCreateForWebContents(other);
  tabs::TabInitialUrlHelper::FromWebContents(other)->SetUserInitialUrl(
      GURL("https://example.test/start"));
  ASSERT_TRUE(tabs::CanReloadInitialUrlForContents(other));
  ASSERT_FALSE(tabs::CanReloadInitialUrlForContents(owner))
      << "the owning tab must stay ineligible or this proves nothing";

  // Rebuilt menu (menus rebuild on every open) now enables the item.
  ui::SimpleMenuModel parent2(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> menu2 =
      tabs::MaybeAppendInitialUrlSubMenu(&parent2, owner);
  ASSERT_NE(nullptr, menu2);
  EXPECT_TRUE(menu2->IsEnabledAt(3))
      << "still disabled although another tab in the window is eligible — the "
         "predicate is scoped to the clicked tab instead of the window";
}

// DEFECT IT EXCLUDES: a hard-coded accelerator, and a correct accelerator that
// Cocoa never renders. Asserting only that the display "changed" after a rebind
// passes with a broken Carbon->KeyboardCode conversion, since a wrong cast
// still yields two different wrong answers — so assert EXACT values.
IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlEditTest,
                       RefreshAllAcceleratorIsExactAndFollowsRebind) {
  ui::SimpleMenuModel parent(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> menu =
      tabs::MaybeAppendInitialUrlSubMenu(&parent, active_contents());
  ASSERT_NE(nullptr, menu);

  // Without the force-show bit a Cocoa context menu renders NO accelerator,
  // however correct GetAcceleratorForCommandId is — and the chord being visible
  // is this issue's entire purpose. Independent of the value assertions below.
  EXPECT_TRUE(menu->GetForceShowAcceleratorForItemAt(3))
      << "the accelerator will not render in a Cocoa context menu";

  ui::Accelerator accel;
  ASSERT_TRUE(menu->GetAcceleratorAt(3, &accel));
  EXPECT_EQ(ui::VKEY_R, accel.key_code());
  EXPECT_EQ(ui::EF_COMMAND_DOWN | ui::EF_CONTROL_DOWN | ui::EF_ALT_DOWN,
            accel.modifiers());

  // Rebind, rebuild, and assert the EXACT new chord — not merely that it moved.
  const tabs::RoamuxShortcut* row = nullptr;
  for (const tabs::RoamuxShortcut& entry : tabs::AllShortcuts()) {
    // The registry keys on the CHROME command id (33013), not this menu's
    // submenu id (2113) — the same two-id-space trap that hid the production
    // bug this test caught.
    if (entry.command_id == IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS) {
      row = &entry;
      break;
    }
  }
  ASSERT_TRUE(row);
  tabs::StoreRebind(browser()->profile()->GetPrefs(), *row,
                    tabs::Chord{.cmd = true,
                                .shift = true,
                                .ctrl = false,
                                .opt = false,
                                .keycode = 0x11});  // kVK_ANSI_T

  ui::SimpleMenuModel parent2(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> menu2 =
      tabs::MaybeAppendInitialUrlSubMenu(&parent2, active_contents());
  ASSERT_NE(nullptr, menu2);
  ui::Accelerator rebound;
  ASSERT_TRUE(menu2->GetAcceleratorAt(3, &rebound));
  EXPECT_EQ(ui::VKEY_T, rebound.key_code())
      << "the menu shows a hard-coded chord instead of the registry's current "
         "binding, or the Carbon->KeyboardCode conversion is wrong";
  EXPECT_EQ(ui::EF_COMMAND_DOWN | ui::EF_SHIFT_DOWN, rebound.modifiers());
}

// DEFECT IT EXCLUDES: the item silently surviving its own kill-switch, and a
// flag-off path that removes more than it should.
//
// The override lives in the FIXTURE CONSTRUCTOR, not the test body:
// BrowserTestBase DCHECKs that FeatureList overrides happen before SetUp()
// runs, so a ScopedFeatureList inside a browsertest body — fine in a unit test
// — crashes the process here.
class RoamuxInitialUrlRefreshAllFlagOffTest : public RoamuxInitialUrlEditTest {
 public:
  RoamuxInitialUrlRefreshAllFlagOffTest() {
    flag_off_.InitAndDisableFeature(features::kRefreshAllInitialUrls);
  }

 private:
  base::test::ScopedFeatureList flag_off_;
};

IN_PROC_BROWSER_TEST_F(RoamuxInitialUrlRefreshAllFlagOffTest,
                       RemovesOnlyThatItem) {
  ui::SimpleMenuModel parent(nullptr);
  std::unique_ptr<ui::SimpleMenuModel> menu =
      tabs::MaybeAppendInitialUrlSubMenu(&parent, active_contents());
  ASSERT_NE(nullptr, menu);
  EXPECT_EQ(2u, menu->GetItemCount())
      << "flag-off must drop the separator and the item, and nothing else";
  EXPECT_EQ(tabs::kEditInitialUrlCommandId, menu->GetCommandIdAt(0));
  EXPECT_EQ(tabs::kSetInitialUrlToCurrentPageCommandId,
            menu->GetCommandIdAt(1));
}

}  // namespace roamux
