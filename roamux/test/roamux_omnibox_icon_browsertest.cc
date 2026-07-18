// SPDX-License-Identifier: Apache-2.0
// roam-159: the omnibox chip on chrome:// URLs renders the Roamux mark.
//
// ChromeLocationBarModelDelegate::GetVectorIconOverride() decides the chip's
// vector icon; patch 0039 swaps its kChromeUIScheme arm from the tinted
// Chromium product glyph to roamux::kRoamuxProductIcon (fixed-colour rays —
// PATH_COLOR_ARGB overrides the security-chip tint at paint time, so the mark
// is identical in light and dark). This test proves the decision point in a
// live browser, mirroring upstream's
// chrome_location_bar_model_delegate_browsertest.cc access pattern (a local
// concrete delegate subclass — the delegate is abstract), and pins the
// no-regression criterion: http/https pages get no override (nullptr), so
// security-chip behaviour is untouched.
//
// RED before 0039 lands in the build: the chrome:// arm returns
// omnibox::kProductChromeRefreshIcon.

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/toolbar/chrome_location_bar_model_delegate.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/omnibox/browser/vector_icons.h"
#include "content/public/test/browser_test.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "roamux/browser/ui/icons/vector_icons.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/gfx/vector_icon_types.h"
#include "url/gurl.h"

namespace {

// The delegate is abstract (GetActiveWebContents is pure); the upstream
// browsertest's local concrete subclass is the established access pattern.
class TestChromeLocationBarModelDelegate
    : public ChromeLocationBarModelDelegate {
 public:
  explicit TestChromeLocationBarModelDelegate(Browser* browser)
      : browser_(browser) {}

  content::WebContents* GetActiveWebContents() const override {
    return browser_->tab_strip_model()->GetActiveWebContents();
  }

 private:
  const raw_ptr<Browser> browser_;
};

}  // namespace

class RoamuxOmniboxIconBrowserTest : public roamux::test::RoamuxBrowserTest {
 protected:
  RoamuxOmniboxIconBrowserTest()
      : https_server_(net::EmbeddedTestServer::TYPE_HTTPS) {}

  void SetUpOnMainThread() override {
    roamux::test::RoamuxBrowserTest::SetUpOnMainThread();
    delegate_ = std::make_unique<TestChromeLocationBarModelDelegate>(browser());
    embedded_test_server()->ServeFilesFromSourceDirectory("chrome/test/data");
    ASSERT_TRUE(embedded_test_server()->Start());
    https_server_.ServeFilesFromSourceDirectory("chrome/test/data");
    ASSERT_TRUE(https_server_.Start());
  }

  void TearDownOnMainThread() override {
    delegate_.reset();
    roamux::test::RoamuxBrowserTest::TearDownOnMainThread();
  }

  const gfx::VectorIcon* IconOverrideAfterNavigating(const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
    return delegate_->GetVectorIconOverride();
  }

  net::EmbeddedTestServer& https_server() { return https_server_; }

 private:
  net::EmbeddedTestServer https_server_;
  std::unique_ptr<TestChromeLocationBarModelDelegate> delegate_;
};

IN_PROC_BROWSER_TEST_F(RoamuxOmniboxIconBrowserTest,
                       ChromeSchemeChipIsRoamuxIcon) {
  const gfx::VectorIcon* icon =
      IconOverrideAfterNavigating(GURL("chrome://version/"));
  ASSERT_NE(nullptr, icon);
  EXPECT_EQ(&roamux::kRoamuxProductIcon, icon)
      << "chrome:// chip icon is '" << icon->name
      << "', not the Roamux mark (still the Chromium glyph?)";
  EXPECT_NE(&omnibox::kProductChromeRefreshIcon, icon);
}

IN_PROC_BROWSER_TEST_F(RoamuxOmniboxIconBrowserTest, WebSchemesGetNoOverride) {
  // http and https pages ride the security-chip machinery untouched — no
  // override. (Error/security *states* are covered by the tier-1 sync proof:
  // patch 0039 never touches those paths.)
  EXPECT_EQ(nullptr, IconOverrideAfterNavigating(
                         embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(nullptr,
            IconOverrideAfterNavigating(https_server().GetURL("/title1.html")));
}
