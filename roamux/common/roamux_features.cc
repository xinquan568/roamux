// SPDX-License-Identifier: Apache-2.0
#include "roamux/common/roamux_features.h"

namespace roamux::features {

BASE_FEATURE(kTabStripPosition,
             "RoamuxTabStripPosition",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-185: shipped default-on
BASE_FEATURE(kInitialUrl,
             "RoamuxInitialUrl",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-187: shipped default-on
BASE_FEATURE(kEdgeImport,
             "RoamuxEdgeImport",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-190: shipped default-on
// roam-208: shipped default-on for v0.0.1-alpha.8
// (chrome://flags/#roamux-bookmark-subfolder-groups lets users opt out).
BASE_FEATURE(kBookmarkSubfolderGroups,
             "RoamuxBookmarkSubfolderGroups",
             base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kTabVisitNav,
             "RoamuxTabVisitNav",
             base::FEATURE_ENABLED_BY_DEFAULT);  // roam-189: shipped default-on
// roam-266: shipped default-on for v0.0.1-alpha.9
// (chrome://flags/#roamux-brave-style-profiles lets users opt out — patch 0064
// adds that entry in the same change; graduating without it would have made
// this the only default-on flag with no kill-switch). Flag-on suppresses the
// default sign-in surfaces (sign-in stays inert-with-explanation unless the
// build is keyed and authorized) and takes profile creation down the name-only
// path; roamux-signin-opt-in re-exposes the Chromium sign-in/sync surfaces.
BASE_FEATURE(kBraveStyleProfiles,
             "RoamuxBraveStyleProfiles",
             base::FEATURE_ENABLED_BY_DEFAULT);
// roam-179: ENABLED by default — the E8 rebrand's user-visible scheme
// branding ships on (D3); the flag stays as a kill-switch (flag-off identity
// is pinned at the browser level).
BASE_FEATURE(kRoamuxSchemeAlias,
             "RoamuxSchemeAlias",
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
// roam-269: ships default-on, like every Roamux flag since roam-226. Its
// chrome://flags entry (patch 0066) lands in the SAME change and is the
// kill-switch — the roam-266 precedent: a default-on flag without one would be
// the only Roamux feature a user cannot turn off. Claims Ctrl+Opt+Cmd+R by
// default; dispatch is registry-only (no accelerators_cocoa.mm row, per the
// roam-214/patch-0053 precedent).
BASE_FEATURE(kRefreshAllInitialUrls,
             "RoamuxRefreshAllInitialUrls",
             base::FEATURE_ENABLED_BY_DEFAULT);
// §7.3 defaults. Validation (out-of-range yields the DEFAULT, never a clip;
// then min_spacing = min(min_spacing, interval)) lives in the pure scheduler's
// Params::FromMilliseconds — roam-268 — not here.
const base::FeatureParam<int> kRefreshAllInitialUrlsIntervalMs{
    &kRefreshAllInitialUrls, "interval_ms", 5000};
const base::FeatureParam<int> kRefreshAllInitialUrlsMinSpacingMs{
    &kRefreshAllInitialUrls, "min_spacing_ms", 750};

}  // namespace roamux::features
