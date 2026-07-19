// SPDX-License-Identifier: Apache-2.0
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// roam-179 (patch 0042): the URL-directory page brands its title and its
// link/row TEXT as roamux:// while every href stays chrome:// — renderers
// cannot request roamux:// (the 0028 handled-scheme block), so a branded href
// would dead-click. Flag-off serves the upstream page unchanged.

// Waits for the lit app to render its fetched URL list, then returns the
// requested probe as a string. The list arrives over mojo after first paint,
// so poll the shadow DOM rather than asserting immediately.
constexpr char kProbeScript[] = R"((async () => {
  const app = document.querySelector('chrome-urls-app');
  for (let i = 0; i < 200; i++) {
    if (app.shadowRoot.querySelectorAll('ul li a[href]').length > 0) break;
    await new Promise(r => setTimeout(r, 50));
  }
  const title = app.shadowRoot.querySelector('h2').textContent;
  // First real (non-self) link: href carries the destination, textContent
  // the display branding.
  const link = app.shadowRoot.querySelector('ul li a[href]:not([href="#"])');
  // First command-URL row (the section is headed "Command URLs for Debug";
  // rows are plain <li> without an anchor) — must stay chrome://.
  const cmdH2 = [...app.shadowRoot.querySelectorAll('h2')]
      .find(h => h.textContent.startsWith('Command URLs'));
  let cmd = '';
  for (let el = cmdH2 && cmdH2.nextElementSibling; el; el = el.nextElementSibling) {
    if (el.tagName === 'UL') { cmd = el.querySelector('li').textContent; break; }
  }
  return JSON.stringify({
    title: title,
    text: link ? link.textContent : '',
    href: link ? link.getAttribute('href') : '',
    cmd: cmd,
  });
})())";

class RoamuxChromeUrlsBrandingBrowserTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxChromeUrlsBrandingBrowserTest() {
    features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxChromeUrlsBrandingBrowserTest,
                       TitleAndLinkTextAreBrandedHrefsStayChrome) {
  ASSERT_TRUE(
      content::NavigateToURL(web_contents(), GURL("chrome://chrome-urls")));
  const std::string probe =
      content::EvalJs(web_contents(), kProbeScript).ExtractString();
  EXPECT_NE(probe.find("\"title\":\"List of Roamux URLs\""), std::string::npos)
      << probe;
  EXPECT_NE(probe.find("\"text\":\"roamux://"), std::string::npos) << probe;
  EXPECT_NE(probe.find("\"href\":\"chrome://"), std::string::npos) << probe;
  // Command-URL rows stay UNBRANDED even with the flag on (plan D4: they are
  // type-only debug URLs, rendered without an anchor).
  EXPECT_NE(probe.find("\"cmd\":\"chrome://"), std::string::npos) << probe;
}

class RoamuxChromeUrlsBrandingFlagOffBrowserTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxChromeUrlsBrandingFlagOffBrowserTest() {
    features_.InitAndDisableFeature(features::kRoamuxSchemeAlias);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxChromeUrlsBrandingFlagOffBrowserTest,
                       FlagOffServesUpstreamPage) {
  content::WebContents* wc =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(content::NavigateToURL(wc, GURL("chrome://chrome-urls")));
  const std::string probe = content::EvalJs(wc, kProbeScript).ExtractString();
  EXPECT_NE(probe.find("\"title\":\"List of Chrome URLs\""), std::string::npos)
      << probe;
  EXPECT_EQ(probe.find("roamux://"), std::string::npos) << probe;
}

}  // namespace
}  // namespace roamux
