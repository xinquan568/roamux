// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/tab_visit/visit_url_filter.h"

#include <string_view>

#include "url/gurl.h"

namespace roamex::tab_visit {

bool IsRecordableVisitUrl(const GURL& url) {
  if (url.is_empty() || !url.is_valid() || url.IsAboutBlank()) {
    return false;
  }
  if (url.SchemeIs("chrome-search")) {
    return false;  // Local NTP / instant.
  }
  if (url.SchemeIs("chrome")) {
    const std::string_view host = url.host();
    if (host == "newtab" || host == "new-tab-page" ||
        host == "new-tab-page-third-party") {
      return false;
    }
  }
  return true;
}

}  // namespace roamex::tab_visit
