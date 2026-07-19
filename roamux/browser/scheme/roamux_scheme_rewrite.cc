// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/scheme/roamux_scheme_rewrite.h"

#include <string_view>

#include "base/feature_list.h"
#include "content/public/common/url_constants.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_url_constants.h"
#include "url/gurl.h"

namespace roamux {

namespace {

// The path-override map (roam-91, generalized by roam-179). Rows exist ONLY
// for hosts whose chrome:// target differs by more than the scheme
// (roamux://about → chrome://settings/help, roam-140). Every other host takes
// the generic scheme-only swap below — roamux://X → chrome://X can never
// shadow a real chrome:// host because the host is untouched. Hosts that
// don't exist as WebUIs (roamux://no-such-host) rewrite to their chrome://
// form and fail exactly as the typed chrome:// URL would; renderer-initiated
// roamux:// stays blocked by the handled-scheme registration (patch 0028
// lists "roamux" in ProfileIOData::IsHandledProtocol), never the OS
// external-protocol path.
struct AliasRow {
  std::string_view roamux_host;
  std::string_view chrome_host;
  // chrome:// path override — non-empty for rows whose target is a settings
  // sub-page rather than a bare WebUI host.
  std::string_view chrome_path;
};
constexpr AliasRow kAliasMap[] = {
    {kRoamuxAliasAboutHost, "settings", "/help"}, // roamux://about → settings/help
};

} // namespace

bool MaybeRewriteRoamuxAliasURL(GURL *url,
                                content::BrowserContext *browser_context) {
  // Checked per call (not at registration) so feature toggles behave
  // predictably; the handler itself is registered unconditionally.
  if (!base::FeatureList::IsEnabled(features::kRoamuxSchemeAlias)) {
    return false;
  }
  if (!url->SchemeIs(kRoamuxScheme)) {
    return false;
  }
  GURL::Replacements replacements;
  replacements.SetSchemeStr(content::kChromeUIScheme);
  for (const AliasRow &row : kAliasMap) {
    if (url->GetHost() == row.roamux_host) {
      replacements.SetHostStr(row.chrome_host);
      if (!row.chrome_path.empty()) {
        replacements.SetPathStr(row.chrome_path);
      }
      break;
    }
  }
  // roam-179: hosts without an override row take the scheme-only swap —
  // host/path/ref preserved.
  *url = url->ReplaceComponents(replacements);
  // Chaining idiom (browser_about_handler.cc precedent): having re-written the
  // URL, let the later chrome: handlers process it.
  return false;
}

} // namespace roamux
