// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_DISPLAY_H_
#define ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_DISPLAY_H_

#include <string>

class GURL;

namespace roamux {

// roam-179: presentation-only scheme branding. Given the display URL `url`
// (the navigation entry's virtual URL) and its formatted omnibox text, returns
// the text with a leading "chrome://" rebranded to "roamux://" — only when
// kRoamuxSchemeAlias is enabled, `url` really is a chrome:// URL, and the
// formatted text still carries the scheme (elided forms fall through
// unbranded, never corrupted). The committed URL/origin is untouched; the
// branded text re-parses to the same destination via the forward rewrite in
// roamux_scheme_rewrite.h, which is exactly the
// FormattedStringWithEquivalentMeaning contract of the hook site (patch 0041).
std::u16string MaybeBrandFormattedUrlForDisplay(const GURL &url,
                                                std::u16string formatted);

} // namespace roamux

#endif // ROAMUX_BROWSER_SCHEME_ROAMUX_SCHEME_DISPLAY_H_
