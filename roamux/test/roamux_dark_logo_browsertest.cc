// SPDX-License-Identifier: Apache-2.0
// roam-169: dark mode must show the Roamux mark, not the Chromium logo.
//
// Two dark-path bypasses exist around roam-158's current-channel-logo repoint:
// (A) the shared cr-toolbar/cr-drawer <picture> dark <source> resolves
//     //resources/images/chrome_logo_dark.svg — this test proves the PACKED
//     payload behind that URL (IDR_WEBUI_IMAGES_CHROME_LOGO_DARK_SVG) is the
//     committed Roamux glyph (roamux/app/resources/theme/chrome_logo_dark.svg,
//     kept in sync with patch 0038 by check_dark_logo.py), not the Chromium
//     art;
// (B) the chrome://version body logo requested chrome://theme/
//     IDR_PRODUCT_LOGO{,_WHITE} (Chromium wordmarks in BOTH themes) — this
//     test proves the served page's DOM now sources the logo from
//     current-channel-logo (the roam-158 chain) and requests no IDR pair.
//
// RED before patch 0038 lands in the build: (A) the pak carries the Chromium
// SVG; (B) the version DOM references the IDR pair.

#include <string>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/webui/resources/grit/webui_resources.h"
#include "url/gurl.h"

namespace {

// The committed payload — the source of truth patch 0038 injects upstream.
constexpr char kCommittedPayload[] =
    "roamux/app/resources/theme/chrome_logo_dark.svg";

// The four Roamux ray colours; the upstream Chromium dark logo has none.
constexpr const char* kPaletteMarkers[] = {"#4285F4", "#EA4335", "#FBBC05",
                                           "#34A853"};

std::string ReadCommittedPayload() {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath src_root;
  if (!base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &src_root)) {
    return std::string();
  }
  std::string contents;
  if (!base::ReadFileToString(src_root.AppendASCII(kCommittedPayload),
                              &contents)) {
    return std::string();
  }
  return contents;
}

}  // namespace

using RoamuxDarkLogoBrowserTest = roamux::test::RoamuxBrowserTest;

// Path A: the packed dark-logo payload is the Roamux glyph.
IN_PROC_BROWSER_TEST_F(RoamuxDarkLogoBrowserTest, PackedDarkLogoIsRoamuxGlyph) {
  const std::string packed =
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
          IDR_WEBUI_IMAGES_CHROME_LOGO_DARK_SVG);
  ASSERT_FALSE(packed.empty()) << "chrome_logo_dark.svg is not packed";

  for (const char* marker : kPaletteMarkers) {
    EXPECT_NE(std::string::npos, packed.find(marker))
        << "packed dark logo lacks the Roamux ray colour " << marker
        << " — still the Chromium art?";
  }
  // The Chromium asset's signature shape: a white-filled clip-path pinwheel.
  EXPECT_EQ(std::string::npos, packed.find("clip-path"))
      << "packed dark logo still carries the Chromium clip-path pinwheel";

  const std::string committed = ReadCommittedPayload();
  ASSERT_FALSE(committed.empty())
      << kCommittedPayload << " is missing or unreadable";
  EXPECT_EQ(committed, packed)
      << "packed dark logo differs from the committed Roamux payload";
}

// Path B: the chrome://version body logo rides the current-channel-logo chain.
IN_PROC_BROWSER_TEST_F(RoamuxDarkLogoBrowserTest,
                       VersionPageBodyLogoIsCurrentChannelLogo) {
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), GURL("chrome://version/")));
  content::WebContents* contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // No element may request the Chromium wordmark IDR pair.
  EXPECT_EQ(0,
            content::EvalJs(contents,
                            "document.querySelectorAll("
                            "    'source[srcset*=\"IDR_PRODUCT_LOGO\"],"
                            "     img[src*=\"IDR_PRODUCT_LOGO\"],"
                            "     img[srcset*=\"IDR_PRODUCT_LOGO\"]').length"))
      << "chrome://version still requests chrome://theme/IDR_PRODUCT_LOGO*";

  // The body logo sources the roam-158 chain.
  EXPECT_EQ(true, content::EvalJs(
                      contents,
                      "!!document.querySelector("
                      "    '#logo img[srcset*=\"current-channel-logo\"]')"))
      << "chrome://version body logo does not use current-channel-logo";
}
