// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/scheme/roamux_scheme_display.h"

#include <string_view>

#include "base/feature_list.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "content/public/common/url_constants.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_url_constants.h"
#include "url/gurl.h"

namespace roamux {

std::u16string MaybeBrandFormattedUrlForDisplay(const GURL &url,
                                                std::u16string formatted) {
  // Checked per call (registration-unconditional idiom, as in
  // roamux_scheme_rewrite.cc) so feature toggles behave predictably.
  if (!base::FeatureList::IsEnabled(features::kRoamuxSchemeAlias)) {
    return formatted;
  }
  // Brand only real chrome:// display URLs; chrome-untrusted:// (a distinct
  // scheme) and everything else pass through.
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return formatted;
  }
  constexpr std::u16string_view kChromePrefix = u"chrome://";
  if (!base::StartsWith(formatted, kChromePrefix)) {
    return formatted;
  }
  return base::StrCat(
      {u"roamux://",
       std::u16string_view(formatted).substr(kChromePrefix.size())});
}

} // namespace roamux
