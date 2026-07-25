// SPDX-License-Identifier: Apache-2.0
#include "roamux/common/roamux_features.h"

namespace roamux::features {

BASE_FEATURE(kTabStripPosition, "RoamuxTabStripPosition",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-185: shipped default-on
BASE_FEATURE(kInitialUrl, "RoamuxInitialUrl",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-187: shipped default-on
BASE_FEATURE(kEdgeImport, "RoamuxEdgeImport",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-190: shipped default-on
// roam-208: shipped default-on for v0.0.1-alpha.8
// (chrome://flags/#roamux-bookmark-subfolder-groups lets users opt out).
BASE_FEATURE(kBookmarkSubfolderGroups,
             "RoamuxBookmarkSubfolderGroups",
             base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kTabVisitNav, "RoamuxTabVisitNav",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-189: shipped default-on
BASE_FEATURE(kBraveStyleProfiles, "RoamuxBraveStyleProfiles",
             base::FEATURE_DISABLED_BY_DEFAULT);
// roam-179: ENABLED by default — the E8 rebrand's user-visible scheme
// branding ships on (D3); the flag stays as a kill-switch (flag-off identity
// is pinned at the browser level).
BASE_FEATURE(kRoamuxSchemeAlias, "RoamuxSchemeAlias",
             base::FEATURE_ENABLED_BY_DEFAULT);
// roam-213: shipped default-on for v0.0.1-alpha.8
// (chrome://flags/#roamux-external-open-profile lets users opt out). Inert
// until a profile is designated — an unset pref still yields no redirect.
BASE_FEATURE(kRoamuxExternalOpenProfile,
             "RoamuxExternalOpenProfile",
             base::FEATURE_ENABLED_BY_DEFAULT);
// roam-214: shipped default-on for v0.0.1-alpha.8
// (chrome://flags/#roamux-tab-strip-toggle-shortcut lets users opt out).
// Claims the Ctrl+Cmd+T chord by default; the strip must be docked
// left/right to arm.
BASE_FEATURE(kTabStripToggleShortcut,
             "RoamuxTabStripToggleShortcut",
             base::FEATURE_ENABLED_BY_DEFAULT);

} // namespace roamux::features
