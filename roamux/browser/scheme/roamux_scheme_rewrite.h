// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_REWRITE_H_
#define ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_REWRITE_H_

class GURL;

namespace content {
class BrowserContext;
} // namespace content

namespace roamux {

// content::BrowserURLHandler forward rewriter (roam-91, generalized by
// roam-179): rewrites roamux://X → chrome://X for EVERY host — scheme-only,
// host/path/ref preserved — with a small path-override map on top
// (roamux://about → chrome://settings/help, roam-140) — when
// features::kRoamuxSchemeAlias is enabled (checked per call, so
// ScopedFeatureList toggles behave predictably). Follows the upstream
// rewrite-then-return-false chaining idiom
// (chrome/browser/browser_about_handler.cc): the URL is mutated in place and
// the function ALWAYS returns false so later handlers (HandleWebUI et al.)
// process the rewritten chrome:// URL. Non-roamux schemes are never touched.
// Registered by patch 0028 in
// ChromeContentBrowserClient::BrowserURLHandlerCreated; the display
// direction (chrome:// shown as roamux://) lives in
// roamux_scheme_display.h (patch 0041).
bool MaybeRewriteRoamuxAliasURL(GURL *url,
                                content::BrowserContext *browser_context);

} // namespace roamux

#endif // ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_REWRITE_H_
