// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_
#define ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_

namespace roamux {

// The product URL scheme ("roamux://…"), roam-91 (value rebranded by
// roam-93). Registered as a standard scheme and marked browser-handled by
// patch 0028 (which uses the annotated string literal at both hook sites — a
// unit test pins literal↔constant agreement).
inline constexpr char kRoamuxScheme[] = "roamux";

// roamux:// path-override host (roam-91/roam-140, generalized by roam-179):
// roamux://about → chrome://settings/help. All other hosts take the generic
// scheme-only swap in roamux_scheme_rewrite.cc; the former kRoamuxAliasFlagsHost
// row is subsumed by that rule.
inline constexpr char kRoamuxAliasAboutHost[] = "about";

} // namespace roamux

#endif // ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_
