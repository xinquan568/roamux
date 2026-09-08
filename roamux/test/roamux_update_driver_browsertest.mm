// SPDX-License-Identifier: Apache-2.0
// roam-287 (grill H11): the issue's userInitiated=NO browsertest — Sparkle's
// PRODUCTION callback showUpdateFoundWithAppcastItem:state:reply: invoked with
// a REAL SPUUserUpdateState (userInitiated=NO — a scheduled check) and a REAL
// SUAppcastItem, through the owner broadcast and the profile's facade to the
// native About row's DOM; then the row's Download click invokes the captured
// reply with Install. The owner is an owner-for-testing (no SPUUpdater), so
// no live Sparkle session exists and the page's auto-check is inert.

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/browser/updates/roamux_update_service.h"
#include "roamux/browser/updates/roamux_update_service_factory.h"
#include "roamux/browser/updates/roamux_update_user_driver.h"
#include "roamux/browser/updates/update_state_machine.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

@interface SPUUserUpdateState (RoamuxDriverTesting)
- (instancetype)initWithStage:(SPUUserUpdateStage)stage
                userInitiated:(BOOL)userInitiated;
@end

namespace roamux::updates {
namespace {

constexpr char kPrelude[] = R"(
    function aboutPage() {
      const ui = document.querySelector('settings-ui');
      if (!ui || !ui.shadowRoot) return null;
      const main = ui.shadowRoot.querySelector('settings-main');
      if (!main || !main.shadowRoot) return null;
      return main.shadowRoot.querySelector('settings-about-page');
    }
    function inAbout(sel) {
      const page = aboutPage();
      return page && page.shadowRoot ?
          page.shadowRoot.querySelector(sel) : null;
    }
    function visible(el) {
      return !!el && !el.hidden && el.offsetParent !== null;
    }
    function statusText() {
      const el = inAbout('#updateStatusMessage');
      return el ? el.textContent.trim() : '';
    }
)";

std::string Poll(const std::string& condition) {
  return base::StringPrintf(
      R"((function() { %s
           return new Promise(resolve => {
             const t0 = Date.now();
             (function poll() {
               let ok = false;
               try { ok = !!(%s); } catch (e) {}
               if (ok) return resolve(true);
               if (Date.now() - t0 > 8000) return resolve(false);
               setTimeout(poll, 50);
             })();
           });
         })())",
      kPrelude, condition.c_str());
}

SUAppcastItem* MakeItem() {
  NSDictionary* dict = @{
    @"sparkle:version" : @"990",
    @"sparkle:shortVersionString" : @"99.0.0-test",
    @"description" : @"notes",
    @"enclosure" : @{
      @"url" : @"https://example.invalid/Roamux-99.zip",
      @"length" : @"1234",
      @"type" : @"application/octet-stream",
    },
  };
  NSString* reason = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  SUAppcastItem* item = [[SUAppcastItem alloc] initWithDictionary:dict
                                                     failureReason:&reason];
#pragma clang diagnostic pop
  EXPECT_NE(nil, item) << "SUAppcastItem refused the fixture: "
                       << base::SysNSStringToUTF8(reason ?: @"(no reason)");
  return item;
}

class RoamuxUpdateDriverBrowserTest : public roamux::test::RoamuxBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    roamux::test::RoamuxBrowserTest::SetUpOnMainThread();
    owner_ = CreateSparkleOwnerForTesting(
        InjectedSparkleStart{/*started=*/true, "", 0, ""});
    RoamuxUpdateServiceFactory::GetInstance()->SetTestingFactory(
        browser()->profile(),
        base::BindRepeating(
            [](SparkleOwner* owner, content::BrowserContext*)
                -> std::unique_ptr<KeyedService> {
              return std::make_unique<RoamuxUpdateService>(owner);
            },
            owner_.get()));
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                             GURL("chrome://settings/help")));
    service_ = RoamuxUpdateServiceFactory::GetForProfile(browser()->profile());
    ASSERT_NE(nullptr, service_);
    web_contents_ = browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_TRUE(Reached("aboutPage() && aboutPage().shadowRoot"));
  }

  [[nodiscard]] bool Reached(const std::string& condition) {
    return content::EvalJs(web_contents_, Poll(condition)).ExtractBool();
  }

  std::unique_ptr<SparkleOwner, SparkleOwnerDeleter> owner_;
  raw_ptr<RoamuxUpdateService, DanglingUntriaged> service_ = nullptr;
  raw_ptr<content::WebContents, DanglingUntriaged> web_contents_ = nullptr;
};

IN_PROC_BROWSER_TEST_F(RoamuxUpdateDriverBrowserTest,
                       ScheduledFindReachesTheRowAndDownloadRepliesInstall) {
  ASSERT_EQ(UpdateStatus::kIdle, service_->snapshot_for_testing().status);
  SUAppcastItem* item = MakeItem();
  ASSERT_NE(nil, item);
  // The fixture is a VALID Sparkle item (version + enclosure) with distinct
  // version / display version, so the driver's choice is observable below.
  EXPECT_EQ("990", base::SysNSStringToUTF8(item.versionString));
  EXPECT_EQ("99.0.0-test", base::SysNSStringToUTF8(item.displayVersionString));
  EXPECT_EQ("notes", base::SysNSStringToUTF8(item.itemDescription));
  EXPECT_NE(nil, item.fileURL);
  EXPECT_EQ(1234u, item.contentLength);
  SPUUserUpdateState* state = [[SPUUserUpdateState alloc]
      initWithStage:SPUUserUpdateStageNotDownloaded
      userInitiated:NO];
  ASSERT_NE(nil, state);
  ASSERT_FALSE(state.userInitiated);

  // Plain locals reached through pointers (the block captures the pointers
  // by value; the RunUntil lambda below may not capture __block variables).
  bool replied = false;
  SPUUserUpdateChoice choice = SPUUserUpdateChoiceDismiss;
  bool* replied_ptr = &replied;
  SPUUserUpdateChoice* choice_ptr = &choice;
  [DriverForTesting(owner_.get())
      showUpdateFoundWithAppcastItem:item
                               state:state
                               reply:^(SPUUserUpdateChoice c) {
                                 *replied_ptr = true;
                                 *choice_ptr = c;
                               }];

  EXPECT_EQ(UpdateStatus::kAvailable, service_->snapshot_for_testing().status);
  EXPECT_EQ("99.0.0-test", service_->snapshot_for_testing().version);
  EXPECT_TRUE(Reached("statusText().includes('99.0.0-test is available')"));
  ASSERT_TRUE(Reached("visible(inAbout('#roamuxDownload'))"));
  EXPECT_FALSE(replied);

  ASSERT_TRUE(content::ExecJs(
      web_contents_, std::string(kPrelude) + "inAbout('#roamuxDownload').click();"));
  EXPECT_TRUE(base::test::RunUntil([&]() { return replied; }));
  EXPECT_EQ(SPUUserUpdateChoiceInstall, choice);
}

}  // namespace
}  // namespace roamux::updates
