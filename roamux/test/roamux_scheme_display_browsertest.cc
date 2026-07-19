// SPDX-License-Identifier: Apache-2.0
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/omnibox/omnibox_view.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/page_type.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// roam-179: presentation-only roamux:// branding of chrome:// URLs (patch
// 0041 hook in ChromeLocationBarModelDelegate + the flag-gated helper in
// //roamux/browser/scheme), plus the renderer-block re-proof under the
// generic roamux://X → chrome://X rewrite.
//
// Assertion vocabulary (roam-91 precedent): NavigationEntry::GetURL() is the
// real committed URL; GetVirtualURL() is the omnibox-visible URL;
// LocationBarModel::GetFormattedFullURL() is the DISPLAYED text (where the
// roam-179 branding lives — the virtual URL itself stays chrome:// for
// browser-initiated chrome:// navigations).
class RoamuxSchemeDisplayBrowserTest : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxSchemeDisplayBrowserTest() {
    features_.InitAndEnableFeature(features::kRoamuxSchemeAlias);
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  content::NavigationEntry* last_entry() {
    return web_contents()->GetController().GetLastCommittedEntry();
  }

  std::u16string displayed_text() {
    return browser()->GetFeatures().location_bar_model()->GetFormattedFullURL();
  }

  base::test::ScopedFeatureList features_;
};

// AC1/AC3 (issue roam-179): a browser-initiated chrome:// navigation — the
// menu → Settings shape, which never enters the roamux:// forward rewrite —
// DISPLAYS as roamux:// while committing chrome://.
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       BrowserInitiatedChromeUrlDisplaysAsRoamux) {
  ASSERT_TRUE(
      content::NavigateToURL(web_contents(), GURL("chrome://version")));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_EQ(content::PAGE_TYPE_NORMAL, entry->GetPageType());
  EXPECT_EQ(GURL("chrome://version/"), entry->GetURL());
  // The virtual URL is untouched (presentation-only invariant)…
  EXPECT_EQ(GURL("chrome://version/"), entry->GetVirtualURL());
  // …the branding lives in the formatted display text.
  EXPECT_EQ(u"roamux://version", displayed_text());
}

// The omnibox edit text round-trips: what the user would edit is roamux://…,
// and that text re-navigates to the same chrome:// destination (the
// FormattedStringWithEquivalentMeaning contract, closed-loop).
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       OmniboxTextIsBrandedAndRoundTrips) {
  ASSERT_TRUE(
      content::NavigateToURL(web_contents(), GURL("chrome://version")));
  OmniboxView* omnibox =
      browser()->window()->GetLocationBar()->GetOmniboxView();
  ASSERT_TRUE(omnibox);
  const std::u16string text = omnibox->GetText();
  EXPECT_EQ(u"roamux://version", text);
  // Round-trip: navigating the displayed text lands on the same destination
  // (virtual URL = the roamux:// form, committed URL = chrome://version).
  const GURL round_trip(base::UTF16ToUTF8(text));
  EXPECT_TRUE(content::NavigateToURL(web_contents(), round_trip));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_EQ(GURL("chrome://version/"), entry->GetURL());
}

// roam-140 regression guard: the about path-override still displays and
// commits as before under the generic rule.
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       AboutOverrideStillLandsOnSettingsHelp) {
  EXPECT_TRUE(content::NavigateToURL(web_contents(), GURL("roamux://about")));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_EQ(GURL("chrome://settings/help"), entry->GetURL());
  EXPECT_EQ(GURL("roamux://about"), entry->GetVirtualURL());
}

// Renderer-block RE-PROOF under the generic rule (Step-5 F1): the curated
// dead-end no longer protects unmapped hosts, so the handled-scheme layer
// (patch 0028: "roamux" in ProfileIOData::IsHandledProtocol → CanRequestURL
// denies renderers) must alone keep web content out of roamux://X.
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       RendererInitiatedGenericHostIsBlocked) {
  ASSERT_TRUE(embedded_test_server()->Start());
  ASSERT_TRUE(content::NavigateToURL(
      web_contents(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_FALSE(content::NavigateToURLFromRenderer(web_contents(),
                                                  GURL("roamux://version")));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_NE(GURL("chrome://version/"), entry->GetURL());
}

// Server redirects to a generic roamux:// host never alias (forward rewriting
// happens at navigation-entry creation only — roam-91 D7, re-proven for the
// generic rule): browser-initiated…
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       ServerRedirectToGenericHostDoesNotAlias) {
  ASSERT_TRUE(embedded_test_server()->Start());
  const GURL redirect =
      embedded_test_server()->GetURL("/server-redirect?roamux://version");
  EXPECT_FALSE(content::NavigateToURL(web_contents(), redirect));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_NE(GURL("chrome://version/"), entry->GetURL());
}

// …and renderer-initiated.
IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayBrowserTest,
                       ServerRedirectFromRendererToGenericHostDoesNotAlias) {
  ASSERT_TRUE(embedded_test_server()->Start());
  ASSERT_TRUE(content::NavigateToURL(
      web_contents(), embedded_test_server()->GetURL("/title1.html")));
  const GURL redirect =
      embedded_test_server()->GetURL("/server-redirect?roamux://version");
  EXPECT_FALSE(content::NavigateToURLFromRenderer(web_contents(), redirect));
  content::NavigationEntry* entry = last_entry();
  ASSERT_TRUE(entry);
  EXPECT_NE(GURL("chrome://version/"), entry->GetURL());
}

// Flag-off identity (kill-switch honesty, roam-179 D3): display stays
// chrome:// and the generic alias is inert — the pre-roam-179 shipped
// behavior, bit-identical.
class RoamuxSchemeDisplayFlagOffBrowserTest
    : public roamux::test::RoamuxBrowserTest {
 public:
  RoamuxSchemeDisplayFlagOffBrowserTest() {
    features_.InitAndDisableFeature(features::kRoamuxSchemeAlias);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxSchemeDisplayFlagOffBrowserTest,
                       FlagOffDisplayAndAliasAreInert) {
  content::WebContents* wc =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_TRUE(content::NavigateToURL(wc, GURL("chrome://version")));
  EXPECT_EQ(
      u"chrome://version",
      browser()->GetFeatures().location_bar_model()->GetFormattedFullURL());
  // Reset to a neutral entry, then prove the generic alias is inert: the
  // handled-scheme dead-end drops roamux://version without committing, so
  // the tab STAYS on about:blank (the roam-91 FlagOffIsInert shape — the
  // last-committed entry must be the neutral one, not a leftover).
  ASSERT_TRUE(content::NavigateToURL(wc, GURL("about:blank")));
  EXPECT_FALSE(content::NavigateToURL(wc, GURL("roamux://version")));
  content::NavigationEntry* entry = wc->GetController().GetLastCommittedEntry();
  ASSERT_TRUE(entry);
  EXPECT_EQ(GURL("about:blank"), entry->GetURL());
  EXPECT_NE(GURL("chrome://version/"), entry->GetURL());
}

}  // namespace
}  // namespace roamux
