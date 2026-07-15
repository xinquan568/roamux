// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_
#define ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_

namespace roamux {

// The product URL scheme ("roamux://…"), roam-91 (value rebranded by
// roam-93). Registered as a standard scheme and marked browser-handled by
// patch 0028 (which uses the annotated string literal at both hook sites — a
// unit test pins literal↔constant agreement).
inline constexpr char kRoamuxScheme[] = "roamux";

// roamux:// alias hosts (roam-91): the curated alias map rewrites
// roamux://about → chrome://settings/help (roam-140) and roamux://flags →
// chrome://flags.
inline constexpr char kRoamuxAliasAboutHost[] = "about";
inline constexpr char kRoamuxAliasFlagsHost[] = "flags";

} // namespace roamux

#endif // ROAMUX_COMMON_ROAMUX_URL_CONSTANTS_H_
