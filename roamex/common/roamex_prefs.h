// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_COMMON_ROAMEX_PREFS_H_
#define ROAMEX_COMMON_ROAMEX_PREFS_H_

class PrefRegistrySimple;

// Local Roamex prefs (plan §7.2 — persisted locally, no sync). `RegisterProfilePrefs` is invoked from
// Chromium's profile-pref registration via a minimal patch hook (roam-3 / §12.2 inventory).
namespace roamex::prefs {

inline constexpr char kTabStripPosition[] = "roamex.tabs.strip_position";                 // E1 (enum: top/bottom/left/right)
inline constexpr char kReopenClosed[] = "roamex.tabs.visit_nav.reopen_closed";            // E4 (Q(new)-C, default off)
inline constexpr char kSigninOptionalEntryPoint[] = "roamex.signin.optional_entry_point_enabled";  // E5 (Q(i3)-C, default off)

void RegisterProfilePrefs(PrefRegistrySimple* registry);

}  // namespace roamex::prefs

#endif  // ROAMEX_COMMON_ROAMEX_PREFS_H_
